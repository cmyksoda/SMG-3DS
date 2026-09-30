#include <smg/Combiner.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
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

// TEV inputs a, b and c see only a register's low eight bits; d sees the whole signed value.
float wrapByte(float v) {
    if (v >= 0 && v <= 1) return v;
    return static_cast<float>(static_cast<int>(std::lround(v * 255)) & 255) / 255;
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
    int constantLast[2]{-1, -1};

    // What each register can hold (color, alpha), signed as GX keeps it; unclamped stages can leave 0..1.
    Range ranges[4][2];

    Compiler(const ModelMaterial& material, bool leadingStage) : m(material), leading(leadingStage) {
        registers[0] = m.tevColors[3];
        for (int i = 0; i < 3; ++i) registers[i + 1] = m.tevColors[i];
        for (int i = 0; i < 4; ++i) {
            const auto& r = registers[i];
            ranges[i][0] = {std::min({r[0], r[1], r[2]}), std::max({r[0], r[1], r[2]})};
            ranges[i][1] = {r[3], r[3]};
        }
        for (auto& row : unitFor) for (int& unit : row) unit = -1;
    }

    Arg reg(int index, bool alphaPart, bool colorInput, bool d) {
        const int part = alphaPart ? 1 : 0;
        const CombineOperand operand = alphaPart ? CombineOperand::Alpha : CombineOperand::Color;
        if (!written[index][part]) {
            const auto& r = registers[index];
            const auto value = [&](float v) { return d ? v : wrapByte(v); };
            if (!colorInput || alphaPart) return constant(value(r[3]));
            return constant(value(r[0]), value(r[1]), value(r[2]));
        }
        if (!ranges[index][part].inUnit()) approximate("unclamped value");
        if (live[part] != index) approximate("overwritten register read");
        return kind(Arg::Prev, operand);
    }

    Arg texture(const TevStage& s, bool alphaPart, bool colorInput) {
        const int texture = s.texMap < 8 ? m.textures[s.texMap] : -1;
        if (texture < 0) return kind(Arg::One);
        const std::uint8_t coord = s.texCoord < 8 ? s.texCoord : 0;
        int& unit = unitFor[s.texMap][coord];
        if (unit < 0) {
            if (program.unitCount == CombinerProgram::MaxUnits) { approximate("more than three textures"); return kind(Arg::One); }
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
        if (op > 1) approximate("compare op"); // compare modes
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
            // Keep the incoming value by letting the stage that made it write the buffer.
            const int maker = first - 1;
            if (maker < 0 || maker > 3 || (saved[part] >= 0 && saved[part] != maker) ||
                constantLast[part] > maker + 1 || index < maker + 2) {
                approximate("previous value needed twice");
                return;
            }
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
            if (index == 0 || (buffer.used && !matches(buffer)) || (saved[part] >= 0 && index > saved[part] + 1))
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

    void emit(const std::vector<Op>& colorOps, const std::vector<Op>& alphaOps) {
        const std::size_t count = std::max(colorOps.size(), alphaOps.size());
        const int first = program.stageCount;
        for (std::size_t i = 0; i < count; ++i) {
            if (program.stageCount == CombinerProgram::MaxStages) { approximate("more than six stages"); return; }
            const int index = program.stageCount;
            auto& stage = program.stages[program.stageCount++];
            Slots colorSlot, alphaSlot;
            const Op pass{CombineFunc::Replace, {kind(Arg::Temp)}};
            Op color = i < colorOps.size() ? colorOps[i] : pass;
            Op alpha = i < alphaOps.size() ? alphaOps[i] : pass;
            if (i >= alphaOps.size()) alpha.args[0].operand = CombineOperand::Alpha;
            stage.colorFunc = color.func;
            stage.alphaFunc = alpha.func;
            stage.colorScale = color.scale;
            stage.alphaScale = alpha.scale;
            for (int k = 0; k < 3; ++k) {
                place(color.args[k], false, index, first, stage, colorSlot, colorBuffer, stage.colorSource[k], stage.colorOperand[k]);
                place(alpha.args[k], true, index, first, stage, alphaSlot, alphaBuffer, stage.alphaSource[k], stage.alphaOperand[k]);
            }
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
    Range stageRange(const TevStage& s, bool alpha) {
        const auto* in = alpha ? s.alphaIn : s.colorIn;
        const Range a = inputRange(s, in[0], alpha, false), b = inputRange(s, in[1], alpha, false);
        const Range d = inputRange(s, in[3], alpha, true);
        const std::uint8_t op = alpha ? s.alphaOp : s.colorOp, bias = alpha ? s.alphaBias : s.colorBias;
        const std::uint8_t scale = alpha ? s.alphaScale : s.colorScale;
        const Range lerp{std::min(a.lo, b.lo), std::max(a.hi, b.hi)};
        Range out = op == 1 ? Range{d.lo - lerp.hi, d.hi - lerp.lo} : Range{d.lo + lerp.lo, d.hi + lerp.hi};
        if (op > 1 || bias == 3) {
            // Compare ops add c or nothing; they are approximated anyway, but later stages read the result.
            out = {d.lo, d.hi + 1};
        } else {
            const float shift = bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0;
            const float factor = scale == 1 ? 2 : scale == 2 ? 4 : scale == 3 ? 0.5f : 1;
            out = {(out.lo + shift) * factor, (out.hi + shift) * factor};
        }
        const bool clamp = alpha ? s.alphaClamp : s.colorClamp;
        const float lo = clamp ? 0 : -1024 / 255.0f, hi = clamp ? 1 : 1023 / 255.0f;
        return {std::clamp(out.lo, lo, hi), std::clamp(out.hi, lo, hi)};
    }

    CombinerProgram run() {
        if (leading) {
            auto& pass = program.stages[program.stageCount++];
            pass.colorSource[0] = pass.alphaSource[0] = CombineSource::Constant;
        }
        for (unsigned i = 0; i < m.tevStageCount; ++i) {
            const auto& s = m.stages[i];
            const auto colorOps = build(colorInput(s, s.colorIn[0]), colorInput(s, s.colorIn[1]), colorInput(s, s.colorIn[2]),
                                        colorInput(s, s.colorIn[3], true), s.colorOp, s.colorBias, s.colorScale, false);
            const auto alphaOps = build(alphaInput(s, s.alphaIn[0]), alphaInput(s, s.alphaIn[1]), alphaInput(s, s.alphaIn[2]),
                                        alphaInput(s, s.alphaIn[3], true), s.alphaOp, s.alphaBias, s.alphaScale, true);
            emit(colorOps, alphaOps);
            const Range results[2]{stageRange(s, false), stageRange(s, true)};
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

CombinerProgram compileTev(const ModelMaterial& material) {
    Compiler direct(material, false);
    const auto program = direct.run();
    if (program.exact || !direct.wantsLeading) return program;
    // Stage 0 cannot read the buffer color; spending a stage lets stage 1 read it instead.
    // A clamp between ops is a smaller error than a wrong constant.
    const auto shifted = Compiler(material, true).run();
    return shifted.exact || std::strcmp(shifted.approximation, "more than two constants") ? shifted : program;
}
} // namespace smg
