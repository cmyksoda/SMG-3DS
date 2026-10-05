#include <smg/Combiner.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

namespace smg {
namespace {
using Rgba = std::array<float, 4>;

// One combiner input before stage allocation. Prev is the GX previous value entering the
// stage; Temp is the result of the op just before, inside the same GX stage.
struct Arg {
    enum Kind : std::uint8_t { Zero, One, Constant, Prev, Temp, Texture, Raster } kind = Zero;
    CombineOperand operand = CombineOperand::Color;
    float value[3]{}; // constant rgb, or the alpha in value[0] for alpha args
    std::uint8_t unit = 0;
    bool operator==(const Arg& o) const {
        if (kind != o.kind) return false;
        if (kind == Constant) return value[0] == o.value[0] && value[1] == o.value[1] && value[2] == o.value[2];
        return operand == o.operand && unit == o.unit;
    }
};

struct Op {
    CombineFunc func = CombineFunc::Replace;
    Arg args[3];
    std::uint8_t scale = 1;
};

Arg constant(float r, float g, float b) {
    Arg a;
    a.kind = Arg::Constant;
    a.value[0] = r; a.value[1] = g; a.value[2] = b;
    return a;
}
Arg constant(float v) { return constant(v, v, v); }
Arg kind(Arg::Kind k, CombineOperand operand = CombineOperand::Color) {
    Arg a;
    a.kind = k;
    a.operand = operand;
    return a;
}

CombineOperand inverted(CombineOperand operand) {
    const auto value = static_cast<std::uint8_t>(operand);
    return static_cast<CombineOperand>(value ^ 1);
}

Arg invert(Arg a) {
    if (a.kind == Arg::Zero) return kind(Arg::One);
    if (a.kind == Arg::One) return kind(Arg::Zero);
    if (a.kind == Arg::Constant) return constant(1 - a.value[0], 1 - a.value[1], 1 - a.value[2]);
    a.operand = inverted(a.operand);
    return a;
}

bool zero(const Arg& a) {
    return a.kind == Arg::Zero || (a.kind == Arg::Constant && !a.value[0] && !a.value[1] && !a.value[2]);
}
bool one(const Arg& a) {
    return a.kind == Arg::One || (a.kind == Arg::Constant && a.value[0] == 1 && a.value[1] == 1 && a.value[2] == 1);
}
bool usesPrev(const Arg& a) { return a.kind == Arg::Prev; }
bool inUnit(const Arg& a) {
    if (a.kind != Arg::Constant) return true;
    for (float v : a.value) if (v < 0 || v > 1) return false;
    return true;
}

// TEV inputs a, b and c see only a register's low eight bits; d sees the whole signed value. Modulo 256 steps, kept
// continuous like the rest of this arithmetic (a computed value need not be a whole step).
float wrapByte(float v) {
    if (v >= 0 && v <= 1) return v;
    return v - 256 / 255.0f * std::floor(v * 255 / 256 + 1e-6f);
}

struct Range {
    float lo = 0, hi = 1;
    bool inUnit() const { return lo >= -1e-6f && hi <= 1 + 1e-6f; }
};

float konst(const ModelMaterial& m, std::uint8_t select, int component) {
    if (select < 8) return (8 - select) / 8.0f;
    if (select >= 0x0c && select < 0x10) return m.konstColors[select - 0x0c][component];
    if (select >= 0x10 && select < 0x20) return m.konstColors[select & 3][(select - 0x10) >> 2];
    return 1;
}

// Channel selection through a swap table: all-equal selections become a broadcast operand.
CombineOperand swapped(const std::uint8_t table[4], bool alphaInput, bool& exact) {
    const CombineOperand channels[]{CombineOperand::Red, CombineOperand::Green, CombineOperand::Blue, CombineOperand::Alpha};
    if (alphaInput) return channels[table[3]];
    if (table[0] == 0 && table[1] == 1 && table[2] == 2) return CombineOperand::Color;
    if (table[0] == table[1] && table[1] == table[2]) return channels[table[0]];
    exact = false;
    return CombineOperand::Color;
}

struct Compiler {
    const ModelMaterial& m;
    CombinerProgram program;
    // GX PREV, C0, C1, C2: a known constant until a stage writes them.
    Rgba registers[4];
    bool written[4][2]{};
    int live[2] = {-1, -1}; // which register the combiner's previous output currently holds
    int unitFor[16][8];

    void approximate(const char* reason) {
        if (program.exact) program.approximation = reason;
        program.exact = false;
    }

    bool leading;           // stage 0 is a pass-through so stage 1 can already read the buffer color
    bool wantsLeading = false;
    int saved[2]{-1, -1};   // the stage whose output the buffer keeps, per color and alpha
    int firstSaved[2]{-1, -1}, savedRead[2]{-1, -1};  // the first such stage; the last stage reading the kept output
    int constantLast[2]{-1, -1};

    // What each register can hold (color, alpha), signed as GX keeps it; unclamped stages can leave 0..1.
    Range ranges[4][2];

    bool knownConstants;
    bool color1FromUnit;
    int paletteUnit = -1;

    Compiler(const ModelMaterial& material, bool leadingStage, bool known, bool color1)
        : m(material), leading(leadingStage), knownConstants(known), color1FromUnit(color1) {
        registers[0] = m.tevColors[3];
        for (int i = 0; i < 3; ++i) registers[i + 1] = m.tevColors[i];
        for (int i = 0; i < 4; ++i) {
            const auto& r = registers[i];
            ranges[i][0] = {std::min({r[0], r[1], r[2]}), std::max({r[0], r[1], r[2]})};
            ranges[i][1] = {r[3], r[3]};
        }
        for (auto& row : unitFor) for (int& unit : row) unit = -1;
        if (color1FromUnit) ranges[2][0] = ranges[2][1] = {0, 1};
    }

    Arg reg(int index, bool alphaPart, bool colorInput, bool d) {
        const int part = alphaPart ? 1 : 0;
        if (index == 2 && color1FromUnit && !written[2][part]) return palette(alphaPart, colorInput);
        const CombineOperand operand = alphaPart ? CombineOperand::Alpha : CombineOperand::Color;
        if (!written[index][part]) {
            const auto& r = registers[index];
            const auto value = [&](float v) { return d ? v : wrapByte(v); };
            if (!colorInput || alphaPart) return constant(value(r[3]));
            return constant(value(r[0]), value(r[1]), value(r[2]));
        }
        const Range& known = ranges[index][part];
        // A register whose value every input fixed is a constant, however the combiner has moved on.
        if (knownConstants && known.lo == known.hi) {
            return constant(d ? known.lo : wrapByte(known.lo));
        }
        if (!known.inUnit()) approximate("unclamped value");
        if (live[part] != index) approximate("overwritten register read");
        return kind(Arg::Prev, operand);
    }

    Arg palette(bool alphaPart, bool colorInput) {
        if (paletteUnit < 0) {
            if (program.unitCount == CombinerProgram::MaxUnits) {
                approximate("no unit left for C1");
                return kind(Arg::Zero);
            }
            paletteUnit = static_cast<int>(program.unitCount++);
            program.units[paletteUnit] = {CombinerProgram::Palette, 0};
        }
        Arg a = kind(Arg::Texture, alphaPart || !colorInput ? CombineOperand::Alpha : CombineOperand::Color);
        a.unit = static_cast<std::uint8_t>(paletteUnit);
        return a;
    }

    Arg texture(const TevStage& s, bool alphaPart, bool colorInput) {
        const int texture = s.texMap < 8 ? m.textures[s.texMap] : -1;
        if (texture < 0) return kind(Arg::One);
        const std::uint8_t coord = s.texCoord < 8 ? s.texCoord : 0;
        int& unit = unitFor[s.texMap][coord];
        if (unit < 0 && program.unitCount == CombinerProgram::MaxUnits) {
            // A multi-tap filter (a blur's six taps of one map) keeps its weight on a unit already sampling the map;
            // read as one, each extra tap added white.
            for (unsigned u = 0; u < program.unitCount && unit < 0; ++u)
                if (program.units[u].texture == texture) unit = static_cast<int>(u);
            approximate("more than three textures");
            if (unit < 0) return kind(Arg::One);
        }
        if (unit < 0) {
            unit = program.unitCount++;
            program.units[unit] = {texture, coord};
        }
        bool exact = true;
        Arg a = kind(Arg::Texture, swapped(m.swapTables[s.texSwap], alphaPart, exact));
        if (!exact) approximate("channel swap");
        if (!colorInput && a.operand == CombineOperand::Color) a.operand = CombineOperand::Alpha;
        a.unit = unit;
        return a;
    }

    Arg raster(const TevStage& s, bool alphaPart, bool colorInput) {
        if (s.channel == 0xff || s.channel == 6) return kind(Arg::Zero);
        if (s.channel == 1 || s.channel == 3 || s.channel == 5) approximate("second color channel");
        bool exact = true;
        Arg a = kind(Arg::Raster, swapped(m.swapTables[s.rasSwap], alphaPart, exact));
        if (!exact) approximate("channel swap");
        if (!colorInput && a.operand == CombineOperand::Color) a.operand = CombineOperand::Alpha;
        return a;
    }

    Arg colorInput(const TevStage& s, std::uint8_t input, bool d = false) {
        switch (input) {
        case 0: case 2: case 4: case 6: return reg(input / 2, false, true, d);
        case 1: case 3: case 5: case 7: return reg(input / 2, true, true, d);
        case 8: return texture(s, false, true);
        case 9: return texture(s, true, true);
        case 10: return raster(s, false, true);
        case 11: return raster(s, true, true);
        case 12: return kind(Arg::One);
        case 13: return constant(0.5f);
        case 14: return constant(konst(m, s.konstColor, 0), konst(m, s.konstColor, 1), konst(m, s.konstColor, 2));
        default: return kind(Arg::Zero);
        }
    }

    Arg alphaInput(const TevStage& s, std::uint8_t input, bool d = false) {
        switch (input) {
        case 0: case 1: case 2: case 3: return reg(input, true, false, d);
        case 4: return texture(s, true, false);
        case 5: return raster(s, true, false);
        case 6: return constant(konst(m, s.konstAlpha, 3));
        default: return kind(Arg::Zero);
        }
    }

    enum Form { None, Single, Product, Lerp };

    // out = (d +/- lerp(a, b, c) + bias) * scale, as a short list of combiner ops.
    std::vector<Op> build(Arg a, Arg b, Arg c, Arg d, std::uint8_t op, std::uint8_t bias, std::uint8_t scale, bool alpha) {
        std::vector<Op> ops;
        const Arg temp = kind(Arg::Temp, alpha ? CombineOperand::Alpha : CombineOperand::Color);
        const bool subtract = op == 1;
        if (op > 1) return compare(a, b, c, d, op, alpha);
        Form form;
        Arg x, y;
        if (zero(c) || (a == b)) { form = Single; x = a; }
        else if (one(c)) { form = Single; x = b; }
        else if (zero(a) && zero(b)) { form = None; }
        else if (zero(a)) { form = Product; x = b; y = c; }
        else if (zero(b)) { form = Product; x = a; y = invert(c); }
        else form = Lerp;
        if (form == Single && zero(x)) form = None;
        if (!inUnit(d)) return signedSum(form, x, y, a, b, c, d, subtract, bias, scale, alpha);

        if (form == None) {
            ops.push_back({CombineFunc::Replace, {d}});
        } else if (zero(d) && !subtract) {
            if (form == Single) ops.push_back({CombineFunc::Replace, {x}});
            else if (form == Product) ops.push_back({CombineFunc::Modulate, {x, y}});
            else ops.push_back({CombineFunc::Interpolate, {b, a, c}});
        } else if (form == Single) {
            ops.push_back({subtract ? CombineFunc::Subtract : CombineFunc::Add, {d, x}});
        } else if (form == Product && !subtract) {
            ops.push_back({CombineFunc::MultiplyAdd, {x, y, d}});
        } else if (!subtract && usesPrev(d) && bias != 2 && scale != 3) {
            // d + a(1 - c) + bc reads the incoming previous value in the first op. A signed or halved
            // sum instead needs the whole lerp in one op, and gets d back from the buffer.
            ops.push_back({CombineFunc::MultiplyAdd, {a, invert(c), d}});
            ops.push_back({CombineFunc::MultiplyAdd, {b, c, temp}});
        } else {
            if (form == Product) ops.push_back({CombineFunc::Modulate, {x, y}});
            else ops.push_back({CombineFunc::Interpolate, {b, a, c}});
            ops.push_back({subtract ? CombineFunc::Subtract : CombineFunc::Add, {d, temp}});
        }

        // GX saturates once at the end; a separate op after a sum that passed 1 sees it clipped.
        const auto overflows = [](const Op& op) { return op.func == CombineFunc::Add || op.func == CombineFunc::MultiplyAdd; };
        // A rewrite is exact only if the op it reshapes saw unclipped inputs.
        const auto whole = [&] { return ops.size() == 1 || !overflows(ops[ops.size() - 2]); };
        const Arg half = constant(0.5f);
        if (bias == 1) {
            // d - x + 1/2 is d + (1 - x) - 1/2, one signed add without clamping in between.
            Op& last = ops.back();
            if (last.func == CombineFunc::Replace) last = {CombineFunc::Add, {last.args[0], half}};
            else if (last.func == CombineFunc::Subtract) last = {CombineFunc::AddSigned, {last.args[0], invert(last.args[1])}};
            else {
                if (last.func == CombineFunc::AddSigned) approximate("clamp between ops");
                ops.push_back({CombineFunc::Add, {temp, half}});
            }
        } else if (bias == 2) {
            Op& last = ops.back();
            if (last.func == CombineFunc::Add) last.func = CombineFunc::AddSigned;
            else if (last.func == CombineFunc::Replace) last = {CombineFunc::AddSigned, {last.args[0], kind(Arg::Zero)}};
            else if (last.func == CombineFunc::MultiplyAdd && ops.size() == 1) {
                // pq + d - 1/2 as a product, then a signed add.
                const Arg d = last.args[2];
                last = {CombineFunc::Modulate, {last.args[0], last.args[1]}};
                ops.push_back({CombineFunc::AddSigned, {temp, d}});
            } else {
                if (overflows(last)) approximate("clamp between ops");
                ops.push_back({CombineFunc::Subtract, {temp, half}});
            }
        } else if (bias == 3) {
            approximate("compare bias");
        }
        if (scale == 1 || scale == 2) {
            ops.back().scale = scale == 1 ? 2 : 4;
        } else if (scale == 3) {
            // Halving a sum is interpolating its two terms halfway, with nothing clipped between.
            Op last = ops.back();
            if (last.func == CombineFunc::Add && whole()) {
                ops.back() = {CombineFunc::Interpolate, {last.args[0], last.args[1], half}};
            } else if (last.func == CombineFunc::MultiplyAdd && ops.size() == 1) {
                ops.back() = {CombineFunc::Modulate, {last.args[0], last.args[1]}};
                ops.push_back({CombineFunc::Interpolate, {temp, last.args[2], half}});
            } else if (last.func == CombineFunc::AddSigned && whole()) {
                ops.back() = {CombineFunc::Interpolate, {last.args[0], last.args[1], half}};
                ops.push_back({CombineFunc::Subtract, {temp, constant(0.25f)}});
            } else {
                if (overflows(last) || last.func == CombineFunc::AddSigned) approximate("clamp between ops");
                ops.push_back({CombineFunc::Modulate, {temp, half}});
            }
        }
        return ops;
    }

    // d + (a > b ? c : 0). For eight-bit inputs a > b is (a - b) * 256 saturated: a subtract and two doublings, each
    // times four. Equality and the 16- and 24-bit compares add nothing, leaving an effect out rather than smeared.
    std::vector<Op> compare(Arg a, Arg b, const Arg& c, const Arg& d, std::uint8_t op, bool alpha) {
        const Arg none = kind(Arg::Zero);
        if (zero(c)) return build(none, none, none, d, 0, 0, 0, alpha);
        const auto unsupported = [&] {
            approximate("compare op");
            return build(none, none, none, d, 0, 0, 0, alpha);
        };
        const bool byRed = !alpha && op <= 9;
        if ((op & 1) || (op < 14 && !byRed) || !inUnit(d)) return unsupported();
        if (byRed) {
            for (Arg* x : {&a, &b}) {
                if (x->kind == Arg::Constant) *x = constant(x->value[0]);
                else if (x->operand == CombineOperand::Color) x->operand = CombineOperand::Red;
            }
        }
        const Arg temp = kind(Arg::Temp, alpha ? CombineOperand::Alpha : CombineOperand::Color);
        std::vector<Op> ops{{CombineFunc::Subtract, {a, b}, 4}, {CombineFunc::Add, {temp, temp}, 4}, {CombineFunc::Add, {temp, temp}, 4}};
        if (zero(d)) {
            if (!one(c)) ops.push_back({CombineFunc::Modulate, {temp, c}});
        } else {
            ops.push_back(one(c) ? Op{CombineFunc::Add, {temp, d}} : Op{CombineFunc::MultiplyAdd, {temp, c, d}});
        }
        return program.stageCount + ops.size() > CombinerProgram::MaxStages ? unsupported() : ops;
    }

    // (k +/- t) * scale for a constant k outside 0..1, as GX saturates it: the positive and negative
    // parts of k never meet in one component, so a subtract and an add apply them without clipping.
    std::vector<Op> signedSum(Form form, Arg x, Arg y, Arg a, Arg b, Arg c, Arg d, bool subtract, std::uint8_t bias,
                              std::uint8_t scale, bool alpha) {
        std::vector<Op> ops;
        const Arg temp = kind(Arg::Temp, alpha ? CombineOperand::Alpha : CombineOperand::Color);
        const float factor = scale == 1 ? 2 : scale == 2 ? 4 : scale == 3 ? 0.5f : 1;
        if (bias == 3) approximate("compare bias");
        float k[3], positive[3], negative[3];
        bool beyondOne = false, anyPositive = false, anyNegative = false;
        for (int i = 0; i < 3; ++i) {
            k[i] = d.value[alpha ? 0 : i] + (bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0);
            positive[i] = std::min(std::max(k[i], 0.0f), 1.0f);
            negative[i] = std::min(std::max(-k[i], 0.0f), 1.0f);
            beyondOne |= k[i] > 1;
            anyPositive |= positive[i] > 0;
            anyNegative |= negative[i] > 0;
        }
        const bool mixed = anyPositive && anyNegative;
        if (form == None || (form == Single && (x.kind == Arg::Constant || x.kind == Arg::One))) {
            // Nothing varies: saturate the constant here.
            float v[3];
            for (int i = 0; i < 3; ++i) {
                const float term = form == None ? 0 : x.kind == Arg::One ? 1 : x.value[alpha ? 0 : i];
                v[i] = std::clamp((subtract ? k[i] - term : k[i] + term) * factor, 0.0f, 1.0f);
            }
            ops.push_back({CombineFunc::Replace, {constant(v[0], v[1], v[2])}});
            return ops;
        }
        Arg t;
        if (form == Single) t = x;
        else {
            if (form == Product) ops.push_back({CombineFunc::Modulate, {x, y}});
            else ops.push_back({CombineFunc::Interpolate, {b, a, c}});
            t = temp;
        }
        if (subtract) {
            // k - t: a component with k at or below zero saturates to zero, as max(0, 0 - t) does.
            if (beyondOne) approximate("register beyond one");
            ops.push_back({CombineFunc::Subtract, {constant(positive[0], positive[1], positive[2]), t}});
        } else {
            // A sum past one saturates the same whether k or one was added, unless it is halved after.
            if (beyondOne && factor < 1) approximate("register beyond one");
            bool small = true;
            for (float v : k) small &= std::fabs(v) <= 0.5f;
            if (mixed && small) {
                ops.push_back({CombineFunc::AddSigned, {t, constant(k[0] + 0.5f, k[1] + 0.5f, k[2] + 0.5f)}});
            } else {
                if (anyNegative) {
                    ops.push_back({CombineFunc::Subtract, {t, constant(negative[0], negative[1], negative[2])}});
                    t = temp;
                }
                if (anyPositive) ops.push_back({CombineFunc::Add, {t, constant(positive[0], positive[1], positive[2])}});
            }
        }
        if (factor > 1) {
            ops.back().scale = static_cast<std::uint8_t>(factor);
        } else if (factor < 1) {
            const Op last = ops.back();
            const Arg half = constant(0.5f);
            if (last.func == CombineFunc::Add) {
                ops.back() = {CombineFunc::Interpolate, {last.args[0], last.args[1], half}};
            } else if (last.func == CombineFunc::AddSigned) {
                ops.back() = {CombineFunc::Interpolate, {last.args[0], last.args[1], half}};
                ops.push_back({CombineFunc::Subtract, {temp, constant(0.25f)}});
            } else {
                ops.push_back({CombineFunc::Modulate, {temp, half}});
            }
        }
        return ops;
    }

    struct Slots { bool used = false; float value[3]{}; };

    // Maps an op argument to a combiner source, placing constants in the stage slot or the buffer.
    void place(const Arg& arg, bool alpha, int index, int first, CombineStage& stage, Slots& slot, Slots& buffer,
               CombineSource& source, CombineOperand& operand) {
        // The buffer keeps color and alpha apart; what matters is the component read, not the combiner.
        const bool readsAlpha = arg.operand == CombineOperand::Alpha || arg.operand == CombineOperand::OneMinusAlpha;
        const int part = arg.kind == Arg::Prev ? (readsAlpha ? 1 : 0) : (alpha ? 1 : 0);
        operand = arg.operand;
        switch (arg.kind) {
        case Arg::Prev: {
            source = CombineSource::Previous;
            if (index == first) return;
            // Keep the incoming value by letting the stage that made it write the buffer. A later stage may write it
            // again once the earlier value's readers are done: a write shows two stages on.
            const int maker = first - 1;
            const bool replaces = saved[part] >= 0 && saved[part] != maker;
            if (maker < 0 || maker > 3 || constantLast[part] > maker + 1 || index < maker + 2 ||
                (replaces && (maker < saved[part] || savedRead[part] > maker + 1))) {
                approximate("previous value needed twice");
                return;
            }
            if (firstSaved[part] < 0) firstSaved[part] = maker;
            savedRead[part] = replaces ? index : std::max(savedRead[part], index);
            saved[part] = maker;
            program.bufferUpdate[part] |= 1 << maker;
            source = CombineSource::Buffer;
            return;
        }
        case Arg::Temp: source = CombineSource::Previous; return;
        case Arg::Texture: source = static_cast<CombineSource>(static_cast<int>(CombineSource::Texture0) + arg.unit); return;
        case Arg::Raster: source = CombineSource::Primary; return;
        default: break;
        }
        float value[3]{0, 0, 0};
        if (arg.kind == Arg::One) value[0] = value[1] = value[2] = 1;
        if (arg.kind == Arg::Constant) for (int i = 0; i < 3; ++i) value[i] = arg.value[i];
        const int count = alpha ? 1 : 3;
        for (int i = 0; i < count; ++i) if (value[i] < 0 || value[i] > 1) approximate("signed constant");
        const auto matches = [&](const Slots& s) {
            for (int i = 0; i < count; ++i) if (s.value[i] != value[i]) return false;
            return true;
        };
        operand = alpha ? CombineOperand::Alpha : CombineOperand::Color;
        if (!slot.used || matches(slot)) {
            slot.used = true;
            for (int i = 0; i < count; ++i) slot.value[i] = value[i];
            if (alpha) stage.constant[3] = value[0];
            else for (int i = 0; i < 3; ++i) stage.constant[i] = value[i];
            source = CombineSource::Constant;
        } else {
            if (index == 0) wantsLeading = true;
            if (index == 0 || (buffer.used && !matches(buffer)) || (firstSaved[part] >= 0 && index > firstSaved[part] + 1))
                approximate("more than two constants");
            constantLast[part] = std::max(constantLast[part], index);
            buffer.used = true;
            for (int i = 0; i < count; ++i) buffer.value[i] = value[i];
            if (alpha) program.bufferColor[3] = value[0];
            else for (int i = 0; i < 3; ++i) program.bufferColor[i] = value[i];
            source = CombineSource::Buffer;
        }
    }

    Slots colorBuffer, alphaBuffer;

    // Each channel's ops follow its own previous ones: an alpha chain longer than its color (or the other way round)
    // doesn't hold the next GX stage's other channel back. Color that reads the incoming alpha starts with it, as the
    // alpha ops after would overwrite what it reads. ready: where each channel's next ops may begin.
    int ready[2] = {0, 0};
    Slots stageSlots[CombinerProgram::MaxStages][2];

    static bool readsAlpha(const std::vector<Op>& ops) {
        for (const Op& op : ops)
            for (const Arg& a : op.args)
                if (a.kind == Arg::Prev && (a.operand == CombineOperand::Alpha || a.operand == CombineOperand::OneMinusAlpha)) return true;
        return false;
    }

    void emit(const std::vector<Op>& colorOps, const std::vector<Op>& alphaOps) {
        int first[2] = {ready[0], ready[1]};
        if (readsAlpha(colorOps)) first[0] = first[1] = std::max(ready[0], ready[1]);
        // Only the arguments a function reads: an unused one, a zero, would take a constant slot.
        const auto arity = [](CombineFunc f) {
            return f == CombineFunc::Replace ? 1 : f == CombineFunc::Interpolate || f == CombineFunc::MultiplyAdd ? 3 : 2;
        };
        for (int part = 0; part < 2; ++part) {
            const auto& ops = part ? alphaOps : colorOps;
            for (std::size_t i = 0; i < ops.size(); ++i) {
                const int index = first[part] + static_cast<int>(i);
                if (index >= static_cast<int>(CombinerProgram::MaxStages)) { approximate("more than six stages"); return; }
                while (static_cast<int>(program.stageCount) <= index) {
                    program.stages[program.stageCount] = CombineStage{};
                    stageSlots[program.stageCount][0] = stageSlots[program.stageCount][1] = Slots{};
                    ++program.stageCount;
                }
                auto& stage = program.stages[index];
                const Op& op = ops[i];
                (part ? stage.alphaFunc : stage.colorFunc) = op.func;
                (part ? stage.alphaScale : stage.colorScale) = op.scale;
                for (int k = 0; k < arity(op.func); ++k)
                    place(op.args[k], part == 1, index, first[part], stage, stageSlots[index][part], part ? alphaBuffer : colorBuffer,
                          part ? stage.alphaSource[k] : stage.colorSource[k], part ? stage.alphaOperand[k] : stage.colorOperand[k]);
            }
            ready[part] = first[part] + static_cast<int>(ops.size());
        }
    }

    Range inputRange(const TevStage& s, std::uint8_t input, bool alpha, bool d) {
        const auto registerRange = [&](int index, int part) {
            const Range r = ranges[index][part];
            if (d || r.inUnit()) return r;
            if (written[index][part]) return Range{};
            const auto& v = registers[index];
            if (part) return Range{wrapByte(v[3]), wrapByte(v[3])};
            return Range{std::min({wrapByte(v[0]), wrapByte(v[1]), wrapByte(v[2])}),
                         std::max({wrapByte(v[0]), wrapByte(v[1]), wrapByte(v[2])})};
        };
        const auto konstRange = [&](std::uint8_t select, bool alphaPart) {
            if (alphaPart) { const float v = konst(m, select, 3); return Range{v, v}; }
            const float r = konst(m, select, 0), g = konst(m, select, 1), b = konst(m, select, 2);
            return Range{std::min({r, g, b}), std::max({r, g, b})};
        };
        if (alpha) {
            if (input < 4) return registerRange(input, 1);
            if (input == 6) return konstRange(s.konstAlpha, true);
            return input == 7 ? Range{0, 0} : Range{};
        }
        if (input < 8) return registerRange(input / 2, input & 1);
        if (input == 12) return {1, 1};
        if (input == 13) return {0.5f, 0.5f};
        if (input == 14) return konstRange(s.konstColor, false);
        return input == 15 ? Range{0, 0} : Range{};
    }

    // Bounds of what a stage leaves in its register: GX saturates only when the stage asks it to.
    Range stageRange(const TevStage& s, bool alpha, bool saturate = true) {
        const auto* in = alpha ? s.alphaIn : s.colorIn;
        const Range a = inputRange(s, in[0], alpha, false), b = inputRange(s, in[1], alpha, false);
        const Range c = inputRange(s, in[2], alpha, false), d = inputRange(s, in[3], alpha, true);
        const std::uint8_t op = alpha ? s.alphaOp : s.colorOp, bias = alpha ? s.alphaBias : s.colorBias;
        const std::uint8_t scale = alpha ? s.alphaScale : s.colorScale;
        // With c fixed the lerp weighs a and b by known amounts; otherwise it can land anywhere between them.
        const Range lerp = c.lo == c.hi ? Range{a.lo * (1 - c.lo) + b.lo * c.lo, a.hi * (1 - c.lo) + b.hi * c.lo}
                                        : Range{std::min(a.lo, b.lo), std::max(a.hi, b.hi)};
        Range out = op == 1 ? Range{d.lo - lerp.hi, d.hi - lerp.lo} : Range{d.lo + lerp.lo, d.hi + lerp.hi};
        if (op > 1 || bias == 3) {
            out = {d.lo, d.hi + c.hi};  // a compare adds c or nothing
        } else {
            const float shift = bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0;
            const float factor = scale == 1 ? 2 : scale == 2 ? 4 : scale == 3 ? 0.5f : 1;
            out = {(out.lo + shift) * factor, (out.hi + shift) * factor};
        }
        if (!saturate) return out;
        const bool clamp = alpha ? s.alphaClamp : s.colorClamp;
        const float lo = clamp ? 0 : -1024 / 255.0f, hi = clamp ? 1 : 1023 / 255.0f;
        return {std::clamp(out.lo, lo, hi), std::clamp(out.hi, lo, hi)};
    }

    // The last stage's value is written out modulo 256. One that stays within a single window of 256 always
    // loses the same amount, returned here (0 when nothing wraps or the window is not fixed).
    static float wrapShift(const Range& r) {
        if (r.inUnit()) return 0;
        const int lo = static_cast<int>(std::floor(std::lround(r.lo * 255) / 256.0));
        const int hi = static_cast<int>(std::floor(std::lround(r.hi * 255) / 256.0));
        return lo == hi ? lo * 256 / 255.0f : 0;
    }
    // Takes the wrap off a constant d, which then carries it exactly; other inputs cannot.
    static bool shiftConstant(Arg& d, float shift) {
        if (d.kind == Arg::Zero || d.kind == Arg::One) {
            d = constant((d.kind == Arg::One ? 1 : 0) - shift);
            return true;
        }
        if (d.kind != Arg::Constant) return false;
        for (float& v : d.value) v -= shift;
        return true;
    }

    // Rewrites in `out` (m's copy) each saturated part every input leaves at 0 or 1 to read that constant: what only it
    // read is then dead. True when one was.
    bool foldFixed(ModelMaterial& out) {
        bool folded = false;
        for (unsigned i = 0; i < m.tevStageCount; ++i) {
            const auto& s = m.stages[i];
            const Range results[2]{stageRange(s, false), stageRange(s, true)};
            TevStage& t = out.stages[i];
            if (s.colorClamp && results[0].lo == results[0].hi && (results[0].lo == 0 || results[0].lo == 1)) {
                t.colorIn[0] = t.colorIn[1] = t.colorIn[2] = 15, t.colorIn[3] = results[0].lo ? 12 : 15;
                t.colorOp = t.colorBias = t.colorScale = 0, folded = true;
            }
            if (s.alphaClamp && results[1].lo == results[1].hi && (results[1].lo == 0 || results[1].lo == 1)) {
                t.alphaIn[0] = t.alphaIn[1] = t.alphaIn[2] = 7, t.alphaIn[3] = results[1].lo ? 6 : 7;
                t.konstAlpha = 0, t.alphaOp = t.alphaBias = t.alphaScale = 0, folded = true;
            }
            const std::uint8_t outputs[2]{static_cast<std::uint8_t>(s.colorReg & 3), static_cast<std::uint8_t>(s.alphaReg & 3)};
            for (int part = 0; part < 2; ++part) written[outputs[part]][part] = true, ranges[outputs[part]][part] = results[part];
        }
        return folded;
    }

    CombinerProgram run() {
        if (leading) {
            auto& pass = program.stages[program.stageCount++];
            pass.colorSource[0] = pass.alphaSource[0] = CombineSource::Constant;
        }
        ready[0] = ready[1] = static_cast<int>(program.stageCount);
        for (unsigned i = 0; i < m.tevStageCount; ++i) {
            const auto& s = m.stages[i];
            Range results[2]{stageRange(s, false), stageRange(s, true)};
            float wraps[2]{0, 0};
            // A sum that reached GX's eleven-bit limit saturated before it wrapped, which a shifted d cannot copy.
            if (i + 1 == m.tevStageCount) {
                for (int part = 0; part < 2; ++part) {
                    const Range raw = stageRange(s, part == 1, false);
                    if (raw.lo >= -1024 / 255.0f && raw.hi <= 1023 / 255.0f) wraps[part] = wrapShift(results[part]);
                }
            }
            Arg d[2]{colorInput(s, s.colorIn[3], true), alphaInput(s, s.alphaIn[3], true)};
            for (int part = 0; part < 2; ++part) {
                // The output is scaled after d is added, so d moves by the shift over the scale.
                const std::uint8_t scale = part ? s.alphaScale : s.colorScale;
                const float factor = scale == 1 ? 2 : scale == 2 ? 4 : scale == 3 ? 0.5f : 1;
                if (!wraps[part] || !shiftConstant(d[part], wraps[part] / factor)) continue;
                results[part] = {results[part].lo - wraps[part], results[part].hi - wraps[part]};
            }
            const auto colorOps = build(colorInput(s, s.colorIn[0]), colorInput(s, s.colorIn[1]), colorInput(s, s.colorIn[2]),
                                        d[0], s.colorOp, s.colorBias, s.colorScale, false);
            const auto alphaOps = build(alphaInput(s, s.alphaIn[0]), alphaInput(s, s.alphaIn[1]), alphaInput(s, s.alphaIn[2]),
                                        d[1], s.alphaOp, s.alphaBias, s.alphaScale, true);
            emit(colorOps, alphaOps);
            // Only one previous value survives in the combiner; older register writes are lost.
            const std::uint8_t outputs[2]{static_cast<std::uint8_t>(s.colorReg & 3), static_cast<std::uint8_t>(s.alphaReg & 3)};
            for (int part = 0; part < 2; ++part) {
                written[outputs[part]][part] = true;
                live[part] = outputs[part];
                ranges[outputs[part]][part] = results[part];
            }
        }
        if (live[0] != 0 || live[1] != 0) approximate("output left in a register");
        // The last stage's value is written out modulo 256, so an unclamped one outside 0..1 wraps.
        if (!ranges[live[0]][0].inUnit() || !ranges[live[1]][1].inUnit()) approximate("unclamped output");
        if (!program.stageCount) {
            program.stageCount = 1;
            program.stages[0].colorSource[0] = program.stages[0].alphaSource[0] = CombineSource::Primary;
        }
        return program;
    }
};
} // namespace

namespace {
CombinerProgram compile(const ModelMaterial& material, bool known, bool color1) {
    Compiler direct(material, false, known, color1);
    const auto program = direct.run();
    if (program.exact || !direct.wantsLeading) return program;
    // Stage 0 cannot read the buffer color; spending a stage lets stage 1 read it instead.
    // A clamp between ops is a smaller error than a wrong constant.
    const auto shifted = Compiler(material, true, known, color1).run();
    return shifted.exact || std::strcmp(shifted.approximation, "more than two constants") ? shifted : program;
}
void clearDead(ModelMaterial& m);
} // namespace

CombinerProgram compileTev(const ModelMaterial& material, bool color1FromUnit) {
    // A register of known value read as a constant frees the previous value but takes a constant slot.
    const auto previous = compile(material, false, color1FromUnit);
    if (previous.exact) return previous;
    const auto constants = compile(material, true, color1FromUnit);
    if (constants.exact) return constants;
    // Stages that only fed a saturated result fixed whatever they give take no combiner stages.
    ModelMaterial folded = material;
    if (!Compiler(material, false, false, color1FromUnit).foldFixed(folded)) return previous;
    clearDead(folded);
    for (const bool known : {false, true})
        if (const auto program = compile(folded, known, color1FromUnit); program.exact) return program;
    return previous;
}
namespace {
// GX inputs by number: color CPREV 0, APREV 1, C0-C2 2/4/6, A0-A2 3/5/7, TEXC 8, TEXA 9, RASC 10, RASA 11, ONE 12,
// HALF 13, KONST 14, ZERO 15; alpha APREV 0, A0-A2 1-3, TEXA 4, RASA 5, KONST 6, ZERO 7.
constexpr std::uint8_t ColorZero = 15, AlphaZero = 7;

void passColor(TevStage& s) {
    s.colorIn[0] = s.colorIn[1] = s.colorIn[2] = ColorZero, s.colorIn[3] = 0;
    s.colorOp = s.colorBias = s.colorScale = 0;
}

// d + a term that reads nothing of PREV, unscaled: a stage the ones before it can be split from.
bool additive(const TevStage& s) {
    return s.colorIn[3] == 0 && s.colorOp == 0 && !s.colorBias && !s.colorScale &&
           std::none_of(s.colorIn, s.colorIn + 3, [](std::uint8_t in) { return in <= 1; });
}

// Stages whose result nothing reads are cleared, so their textures take no unit.
void clearDead(ModelMaterial& m) {
    bool color = true, alpha = true;
    for (int i = int(m.tevStageCount) - 1; i >= 0; --i) {
        TevStage& s = m.stages[i];
        if (!color) std::fill(s.colorIn, s.colorIn + 4, ColorZero), s.colorOp = s.colorBias = s.colorScale = 0;
        if (!alpha) std::fill(s.alphaIn, s.alphaIn + 4, AlphaZero), s.alphaOp = s.alphaBias = s.alphaScale = 0;
        color = std::count(s.colorIn, s.colorIn + 4, 0) > 0;
        alpha = std::count(s.alphaIn, s.alphaIn + 4, 0) || std::count(s.colorIn, s.colorIn + 4, 1);
    }
}

unsigned unitsRead(const ModelMaterial& m) {
    std::set<std::pair<int, int>> read;
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        const TevStage& s = m.stages[i];
        const bool texture = std::any_of(s.colorIn, s.colorIn + 4, [](std::uint8_t in) { return in == 8 || in == 9; }) ||
                             std::count(s.alphaIn, s.alphaIn + 4, 4);
        if (texture && s.texMap < 8 && m.textures[s.texMap] >= 0) read.insert({s.texMap, s.texCoord < 8 ? s.texCoord : 0});
    }
    return unsigned(read.size());
}

// Every pixel with alpha above zero passes: what the equality pass, which adds nothing where alpha is zero, needs.
bool passesAllShown(const ModelMaterial& m) {
    const auto all = [](std::uint8_t compare, std::uint8_t reference) {
        return compare == 7 || (!reference && (compare == 4 || compare == 5 || compare == 6));
    };
    const auto& t = m.alphaTest;
    const bool first = all(t.compare0, t.reference0), second = all(t.compare1, t.reference1);
    return t.op == 1 ? first || second : t.op == 0 && first && second;
}

// The equality compare's pass: color and alpha trade places. Alpha runs the color stages before the compare on their
// reds (texture and raster swapped so alpha reads red and color reads alpha), color runs every alpha stage and then
// takes in the compare's c; drawn where alpha equals the compare's b.
bool equalityPass(const ModelMaterial& m, unsigned k, TevPass& pass) {
    const TevStage& compare = m.stages[k];
    float b;
    switch (compare.colorIn[1]) {
    case 2: case 4: case 6: b = m.tevColors[compare.colorIn[1] / 2 - 1][0]; break;
    case 3: case 5: case 7: b = m.tevColors[compare.colorIn[1] / 2 - 1][3]; break;
    case 12: b = 1; break;
    case 13: b = 0.5f; break;
    case 14: b = konst(m, compare.konstColor, 0); break;
    case 15: b = 0; break;
    default: return false;
    }
    const std::uint8_t c = compare.colorIn[2];
    if (!(c >= 2 && c <= 7) && c != 12 && c != 14) return false;
    if (m.tevStageCount == 16) return false;
    ModelMaterial e = m;
    const std::uint8_t identity[4]{0, 1, 2, 3};
    e.swapTables[1][0] = e.swapTables[1][1] = e.swapTables[1][2] = 3, e.swapTables[1][3] = 0;
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        const TevStage& o = m.stages[i];
        TevStage& s = e.stages[i];
        if (std::memcmp(m.swapTables[o.texSwap], identity, 4) || std::memcmp(m.swapTables[o.rasSwap], identity, 4) || o.alphaOp > 1)
            return false;
        s.texSwap = s.rasSwap = 1;
        static constexpr std::uint8_t toColor[8]{0, 3, 5, 7, 8, 10, 14, 15};
        for (int j = 0; j < 4; ++j) s.colorIn[j] = toColor[o.alphaIn[j]];
        s.colorOp = o.alphaOp, s.colorBias = o.alphaBias, s.colorScale = o.alphaScale, s.colorClamp = o.alphaClamp;
        s.konstColor = o.konstAlpha;
        if (i >= k) {
            s.alphaIn[0] = s.alphaIn[1] = s.alphaIn[2] = AlphaZero, s.alphaIn[3] = 0;
            s.alphaOp = s.alphaBias = s.alphaScale = 0;
            continue;
        }
        if (o.colorOp > 1) return false;
        int select = -1;
        const auto konstFor = [&](int value) { return (select < 0 || select == value) && (select = value, true); };
        for (int j = 0; j < 4; ++j) {
            const std::uint8_t in = o.colorIn[j];
            std::uint8_t& out = s.alphaIn[j];
            if (in == 0) out = 0;
            else if (in == 3 || in == 5 || in == 7) out = std::uint8_t((in - 1) / 2);
            else if (in == 8) out = 4;
            else if (in == 10) out = 5;
            else if (in == 15) out = AlphaZero;
            else if (in == 12 && konstFor(0)) out = 6;
            else if (in == 13 && konstFor(4)) out = 6;
            else if (in == 14 && konstFor(o.konstColor >= 0x0c && o.konstColor < 0x10 ? 0x10 + (o.konstColor - 0x0c) : o.konstColor)) out = 6;
            else return false;
        }
        if (select >= 0) s.konstAlpha = std::uint8_t(select);
        s.alphaOp = o.colorOp, s.alphaBias = o.colorBias, s.alphaScale = o.colorScale, s.alphaClamp = o.colorClamp;
    }
    TevStage& last = e.stages[e.tevStageCount++];
    last = {};
    last.colorIn[0] = ColorZero, last.colorIn[1] = c, last.colorIn[2] = 0, last.colorIn[3] = ColorZero;
    last.alphaIn[0] = last.alphaIn[1] = last.alphaIn[2] = AlphaZero, last.alphaIn[3] = 0;
    last.konstColor = compare.konstColor;
    clearDead(e);
    pass.material = e, pass.alphaEqual = true, pass.reference = std::uint8_t(std::lround(std::clamp(b, 0.0f, 1.0f) * 255));
    return true;
}

// Stage j's term alone, under the whole alpha chain: added times alpha, it is what the stage added to the blend.
TevPass termPass(const ModelMaterial& m, unsigned j) {
    TevPass pass;
    pass.material = m;
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        TevStage& s = pass.material.stages[i];
        if (i < j) std::fill(s.colorIn, s.colorIn + 4, ColorZero), s.colorOp = s.colorBias = s.colorScale = 0;
        else if (i == j) s.colorIn[3] = ColorZero;
        else passColor(s);
    }
    clearDead(pass.material);
    return pass;
}
} // namespace

bool splitTev(const ModelMaterial& m, TevPasses& out) {
    if (m.blend.type != 1 || m.blend.source != 4) return false;  // GX_BM_BLEND from GX_BL_SRCALPHA
    int k = -1;
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        const TevStage& s = m.stages[i];
        if ((s.colorReg & 3) || (s.alphaReg & 3)) return false;
        if (s.colorOp == 9 && k < 0) k = int(i);
        else if (s.colorOp > 1 || s.alphaOp > 1) return false;
    }
    out.first = m, out.extra.clear();
    if (k >= 0) {
        const TevStage& s = m.stages[k];
        if (s.colorIn[0] || s.colorIn[3] || s.colorBias || s.colorScale || !passesAllShown(m)) return false;
        for (unsigned i = unsigned(k) + 1; i < m.tevStageCount; ++i)
            if (!additive(m.stages[i])) return false;
        TevPass equal;
        if (!equalityPass(m, unsigned(k), equal)) return false;
        passColor(out.first.stages[k]);
        out.extra.push_back(equal);
    }
    clearDead(out.first);
    for (int j = int(m.tevStageCount) - 1; j > k && unitsRead(out.first) > CombinerProgram::MaxUnits; --j) {
        const TevStage& s = m.stages[j];
        if (!additive(s)) break;
        if (std::none_of(s.colorIn, s.colorIn + 3, [](std::uint8_t in) { return in == 8 || in == 9; })) continue;
        out.extra.push_back(termPass(m, unsigned(j)));
        passColor(out.first.stages[j]);
        clearDead(out.first);
    }
    if (out.extra.empty() || unitsRead(out.first) > CombinerProgram::MaxUnits) return false;
    for (const TevPass& pass : out.extra)
        if (unitsRead(pass.material) > CombinerProgram::MaxUnits) return false;
    return true;
}
} // namespace smg
