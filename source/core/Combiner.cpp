#include <smg/Combiner.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>
#include <vector>

namespace smg {
namespace {
using Rgba = std::array<float, 4>;

// One combiner input before stage allocation. Prev is the GX previous value entering the
// stage; Temp is the result of the op just before, inside the same GX stage.
struct Arg {
    // Saved: a register's older value, which the combiner stage that made it keeps in the buffer.
    enum Kind : std::uint8_t { Zero, One, Constant, Prev, Temp, Texture, Raster, Saved } kind = Zero;
    CombineOperand operand = CombineOperand::Color;
    float value[3]{}; // constant rgb, or the alpha in value[0] for alpha args
    std::uint8_t unit = 0;  // the texture unit, or for Saved the stage whose output it is
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
    int producer[4][2];  // the combiner stage whose output each register part holds, or -1
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
        for (auto& row : producer) row[0] = row[1] = -1;
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
        if (live[part] == index) return kind(Arg::Prev, operand);
        if (producer[index][part] < 0) {
            approximate("overwritten register read");
            return kind(Arg::Prev, operand);
        }
        Arg older = kind(Arg::Saved, operand);
        older.unit = static_cast<std::uint8_t>(producer[index][part]);
        return older;
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
        // A lerp of constants is one constant, taking one slot instead of up to three.
        const auto fixed = [](const Arg& v) { return v.kind == Arg::Constant || v.kind == Arg::One || v.kind == Arg::Zero; };
        if (form != None && form != Single && fixed(a) && fixed(b) && fixed(c)) {
            const auto value = [](const Arg& v, int i) { return v.kind == Arg::One ? 1.0f : v.kind == Arg::Zero ? 0.0f : v.value[i]; };
            float l[3];
            for (int i = 0; i < 3; ++i) l[i] = value(a, i) * (1 - value(c, i)) + value(b, i) * value(c, i);
            form = Single, x = constant(l[0], l[1], l[2]);
        }
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
            if (b.kind == Arg::Constant && c.kind == Arg::Constant)  // one constant, not two slots
                ops.push_back({CombineFunc::Add, {temp, constant(b.value[0] * c.value[0], b.value[1] * c.value[1], b.value[2] * c.value[2])}});
            else
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

    // lerp(a, b, c) as one term of a chain: an argument as it is, or ops leaving it in temp. Between two constants, a
    // below b, it is c times the slope with a joining the chain's constant.
    Arg lerpTerm(const Arg& a, const Arg& b, const Arg& c, bool alpha, float half, std::vector<Op>& ops) {
        const Arg temp = kind(Arg::Temp, alpha ? CombineOperand::Alpha : CombineOperand::Color);
        if (zero(c) || a == b) return a;
        if (one(c)) return b;
        if (zero(a) && zero(b)) return kind(Arg::Zero);
        const bool rising = std::equal(a.value, a.value + 3, b.value, [](float x, float y) { return x <= y; });
        if (zero(a)) ops.push_back({CombineFunc::Modulate, {b, c}});
        else if (a.kind == Arg::Constant && b.kind == Arg::Constant && c.kind != Arg::Constant && c.kind != Arg::One && rising) {
            ops.push_back({CombineFunc::Modulate, {c, constant(b.value[0] - a.value[0], b.value[1] - a.value[1], b.value[2] - a.value[2])}});
            for (int i = 0; i < 3; ++i) chains[alpha ? 1 : 0].k[i] += a.value[i] * 2 * half;
        } else if (zero(b)) ops.push_back({CombineFunc::Modulate, {a, invert(c)}});
        else ops.push_back({CombineFunc::Interpolate, {b, a, c}});
        return temp;
    }

    // A run of sums into one register, each unclamped and read only as the next one's d, the last clamped and scaled
    // by f: together one sum of terms within 0..1 and a constant k (biases, and the first d when it is constant), so
    // f (terms + k) saturated. Kept at half scale it saturates only where the whole would: a half sum past one means
    // the terms passed two, which f (2 + k) >= 1 saturates anyway (two terms never pass it). The first stage halves its
    // terms, each later one adds half of its own, and the last applies k / 2 and scales by 2f.
    // half[stage]: what the stage's term counts in the half sum, half the scales after it (and its own) multiplied.
    struct Chain { bool active = false; unsigned end = 0; float k[3]{}, factor = 1, half[16]{}; bool dTerm = false; } chains[2];

    // A d that every input leaves fixed: a constant, the konst, or a register not yet written.
    bool constantD(const TevStage& s, bool alpha, float out[3]) const {
        const std::uint8_t in = alpha ? s.alphaIn[3] : s.colorIn[3];
        for (int c = 0; c < 3; ++c) {
            const int component = alpha ? 3 : c;
            if (alpha ? in == 7 : in == 15) out[c] = 0;
            else if (!alpha && in == 12) out[c] = 1;
            else if (!alpha && in == 13) out[c] = 0.5f;
            else if (alpha ? in == 6 : in == 14) out[c] = konst(m, alpha ? s.konstAlpha : s.konstColor, component);
            else if (alpha ? in <= 3 : in <= 7) {
                const int reg = alpha ? in : in / 2, part = alpha || (in & 1) ? 1 : 0;
                if (written[reg][part]) return false;
                out[c] = registers[reg][part ? 3 : c];
            } else {
                return false;
            }
        }
        return true;
    }

    bool planChain(unsigned i, bool alpha, Chain& chain) {
        const auto op = [&](const TevStage& t) { return alpha ? t.alphaOp : t.colorOp; };
        const auto bias = [&](const TevStage& t) { return alpha ? t.alphaBias : t.colorBias; };
        const auto scale = [&](const TevStage& t) { return alpha ? t.alphaScale : t.colorScale; };
        const auto clamped = [&](const TevStage& t) { return bool(alpha ? t.alphaClamp : t.colorClamp); };
        const auto factorOf = [](std::uint8_t code) { return code == 1 ? 2.0f : code == 2 ? 4.0f : code == 3 ? 0.5f : 1.0f; };
        const TevStage& first = m.stages[i];
        if (op(first) || bias(first) == 3 || clamped(first)) return false;
        const int reg = (alpha ? first.alphaReg : first.colorReg) & 3;
        const std::uint8_t mine = static_cast<std::uint8_t>(alpha ? reg : reg * 2);
        bool leaves = !stageRange(first, alpha, false).inUnit();
        unsigned j = i + 1;
        for (; j < m.tevStageCount; ++j) {
            const TevStage& t = m.stages[j];
            const std::uint8_t* in = alpha ? t.alphaIn : t.colorIn;
            if (op(t) || bias(t) == 3 || ((alpha ? t.alphaReg : t.colorReg) & 3) != reg) return false;
            if (in[3] != mine || in[0] == mine || in[1] == mine || in[2] == mine) return false;
            if (alpha && std::find(t.colorIn, t.colorIn + 4, reg * 2 + 1) != t.colorIn + 4) return false;
            if (clamped(t)) break;
            leaves |= !stageRange(t, alpha, false).inUnit();
        }
        if (j >= m.tevStageCount || !leaves) return false;
        const std::uint8_t last = scale(m.stages[j]);
        if (last == 2) return false;  // x4 would need x8 at half scale
        chain.factor = factorOf(last);
        // Each term (and bias) counts times the scales of the stages from its own up to the last's, which ends the sum.
        float weight = 1, most = 0;
        float k[3]{};
        for (unsigned x = j + 1; x-- > i;) {
            if (x < j) weight *= factorOf(scale(m.stages[x]));
            if (weight > 2) return false;  // more than one in the half sum
            chain.half[x] = weight / 2, most += weight / 2;
            for (int c = 0; c < 3; ++c) k[c] += biasValue(bias(m.stages[x])) * weight;
        }
        float d[3]{};
        chain.dTerm = !constantD(first, alpha, d);
        if (chain.dTerm && (!inputRange(first, (alpha ? first.alphaIn : first.colorIn)[3], alpha, true).inUnit() || chain.half[i] < 0.5f)) return false;
        if (chain.dTerm) most += chain.half[i];
        for (int c = 0; c < 3; ++c) k[c] += chain.dTerm ? 0 : d[c] * 2 * chain.half[i];
        // A half sum past one means the whole passed two, which f (2 + k) >= 1 saturates anyway.
        for (int c = 0; c < 3; ++c)
            if (most > 1 && chain.factor * (2 + k[c]) < 1) return false;
        std::copy(k, k + 3, chain.k);
        chain.end = j;
        return chain.active = true;
    }

    // Half a term added to the half sum (the previous output), or the first half sum.
    void halfTerm(const Arg& a, const Arg& b, const Arg& c, bool alpha, bool startsSum, const Arg* d, float weight, std::vector<Op>& ops) {
        const Arg sum = kind(Arg::Prev, alpha ? CombineOperand::Alpha : CombineOperand::Color), half = constant(weight);
        const Arg t = lerpTerm(a, b, c, alpha, weight, ops);
        const bool whole = weight == 1;
        if (startsSum) {
            if (d && zero(t)) ops.push_back(whole ? Op{CombineFunc::Replace, {*d}} : Op{CombineFunc::Modulate, {*d, half}});
            else if (d) ops.push_back(whole ? Op{CombineFunc::Add, {t, *d}} : Op{CombineFunc::Interpolate, {t, *d, half}});
            else if (zero(t)) ops.push_back({CombineFunc::Replace, {kind(Arg::Zero)}});
            else ops.push_back(whole ? Op{CombineFunc::Replace, {t}} : Op{CombineFunc::Modulate, {t, half}});
            return;
        }
        if (t.kind == Arg::One) ops.push_back({CombineFunc::Add, {sum, half}});
        else if (t.kind == Arg::Constant)
            ops.push_back({CombineFunc::Add, {sum, constant(t.value[0] * weight, t.value[1] * weight, t.value[2] * weight)}});
        else if (!zero(t)) ops.push_back(whole ? Op{CombineFunc::Add, {t, sum}} : Op{CombineFunc::MultiplyAdd, {t, half, sum}});
    }

    std::vector<Op> chainStage(const Arg& a, const Arg& b, const Arg& c, const Arg& d, bool alpha, bool startsSum, float weight, const Chain* last) {
        std::vector<Op> ops;
        halfTerm(a, b, c, alpha, startsSum, startsSum && chains[alpha ? 1 : 0].dTerm ? &d : nullptr, weight, ops);
        if (!last) return ops;
        const Arg temp = kind(Arg::Temp, alpha ? CombineOperand::Alpha : CombineOperand::Color);
        const Arg h = ops.empty() ? kind(Arg::Prev, alpha ? CombineOperand::Alpha : CombineOperand::Color) : temp;
        const std::uint8_t scale = static_cast<std::uint8_t>(2 * last->factor);
        float positive[3], negative[3];
        bool anyPositive = false, anyNegative = false, small = true;
        for (int i = 0; i < 3; ++i) {
            const float v = last->k[alpha ? 0 : i] / 2;
            positive[i] = std::clamp(v, 0.0f, 1.0f), negative[i] = std::clamp(-v, 0.0f, 1.0f);
            anyPositive |= v > 0, anyNegative |= v < 0, small &= std::fabs(v) <= 0.5f;
        }
        if (anyPositive && anyNegative && small) {
            ops.push_back({CombineFunc::AddSigned, {h, constant(positive[0] - negative[0] + 0.5f, positive[1] - negative[1] + 0.5f, positive[2] - negative[2] + 0.5f)}, scale});
        } else if (anyPositive && anyNegative) {
            // Apart, as no component has both: one below zero ends there either way.
            ops.push_back({CombineFunc::Subtract, {h, constant(negative[0], negative[1], negative[2])}});
            ops.push_back({CombineFunc::Add, {temp, constant(positive[0], positive[1], positive[2])}, scale});
        } else if (anyNegative) {
            ops.push_back({CombineFunc::Subtract, {h, constant(negative[0], negative[1], negative[2])}, scale});
        } else if (anyPositive) {
            ops.push_back({CombineFunc::Add, {h, constant(positive[0], positive[1], positive[2])}, scale});
        } else {
            ops.push_back({CombineFunc::Replace, {h}, scale});
        }
        return ops;
    }

    static float biasValue(std::uint8_t bias) { return bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0; }
    bool fuseAllowed[2]{true, true}, fusedAny = false, overflowed = false;  // by part: color, alpha

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
        if (subtract && beyondOne) {
            // k - t is (1 - t) + (k - 1) where k passes one and (1 - t) - (1 - k) where it doesn't: a sum past one
            // saturates the same, unless it is halved after.
            if (factor < 1) approximate("register beyond one");
            float above[3], below[3];
            bool anyBelow = false;
            for (int i = 0; i < 3; ++i) {
                above[i] = std::clamp(k[i] - 1, 0.0f, 1.0f), below[i] = std::clamp(1 - k[i], 0.0f, 1.0f);
                anyBelow |= below[i] > 0;
            }
            ops.push_back({CombineFunc::Add, {invert(t), constant(above[0], above[1], above[2])}});
            if (anyBelow) ops.push_back({CombineFunc::Subtract, {temp, constant(below[0], below[1], below[2])}});
        } else if (subtract) {
            // k - t: a component with k at or below zero saturates to zero, as max(0, 0 - t) does.
            ops.push_back({CombineFunc::Subtract, {constant(positive[0], positive[1], positive[2]), t}});
        } else {
            if (beyondOne && factor == 0.5f) {
                // Halved, a k up to two is a constant within one: t / 2 + k / 2, its negative components taken after.
                float up[3], down[3];
                for (int i = 0; i < 3; ++i) up[i] = std::min(std::max(k[i], 0.0f) / 2, 1.0f), down[i] = std::min(std::max(-k[i], 0.0f) / 2, 1.0f);
                ops.push_back({CombineFunc::Modulate, {t, constant(0.5f)}});
                ops.push_back({CombineFunc::Add, {temp, constant(up[0], up[1], up[2])}});
                if (anyNegative) ops.push_back({CombineFunc::Subtract, {temp, constant(down[0], down[1], down[2])}});
                return ops;
            }
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
        const int part = arg.kind == Arg::Prev || arg.kind == Arg::Saved ? (readsAlpha ? 1 : 0) : (alpha ? 1 : 0);
        operand = arg.operand;
        // Keep a value by letting the stage that made it write the buffer. A later stage may write it again once the
        // earlier value's readers are done: a write shows two stages on.
        const auto keep = [&](int maker, const char* otherwise) {
            const bool replaces = saved[part] >= 0 && saved[part] != maker;
            if (maker < 0 || maker > 3 || constantLast[part] > maker + 1 || index < maker + 2 ||
                (replaces && (maker < saved[part] || savedRead[part] > maker + 1))) {
                approximate(otherwise);
                return;
            }
            if (firstSaved[part] < 0) firstSaved[part] = maker;
            savedRead[part] = replaces ? index : std::max(savedRead[part], index);
            saved[part] = maker;
            program.bufferUpdate[part] |= 1 << maker;
            source = CombineSource::Buffer;
        };
        switch (arg.kind) {
        case Arg::Prev:
            source = CombineSource::Previous;
            if (index != first) keep(first - 1, "previous value needed twice");
            return;
        case Arg::Saved:
            source = CombineSource::Previous;
            keep(arg.unit, "overwritten register read");
            return;
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
                if (index >= static_cast<int>(CombinerProgram::MaxStages)) { overflowed = true, approximate("more than six stages"); return; }
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
            // A chain's later stages read the half sum, never d as a register.
            bool continues[2], starts[2];
            for (int part = 0; part < 2; ++part) {
                continues[part] = chains[part].active;
                starts[part] = !continues[part] && fuseAllowed[part] && planChain(i, part == 1, chains[part]);
                fusedAny |= starts[part];
            }
            Arg d[2]{continues[0] ? kind(Arg::Zero) : colorInput(s, s.colorIn[3], true),
                     continues[1] ? kind(Arg::Zero) : alphaInput(s, s.alphaIn[3], true)};
            for (int part = 0; part < 2; ++part) {
                // The output is scaled after d is added, so d moves by the shift over the scale.
                const std::uint8_t scale = part ? s.alphaScale : s.colorScale;
                const float factor = scale == 1 ? 2 : scale == 2 ? 4 : scale == 3 ? 0.5f : 1;
                if (!wraps[part] || !shiftConstant(d[part], wraps[part] / factor)) continue;
                results[part] = {results[part].lo - wraps[part], results[part].hi - wraps[part]};
            }
            const auto ops = [&](bool alpha) {
                const auto input = [&](int k) { return alpha ? alphaInput(s, s.alphaIn[k]) : colorInput(s, s.colorIn[k]); };
                const int part = alpha ? 1 : 0;
                const std::uint8_t bias = alpha ? s.alphaBias : s.colorBias;
                if (continues[part] || starts[part])
                    return chainStage(input(0), input(1), input(2), d[part], alpha, starts[part], chains[part].half[i],
                                      chains[part].end == i ? &chains[part] : nullptr);
                return build(input(0), input(1), input(2), d[part], alpha ? s.alphaOp : s.colorOp, bias, alpha ? s.alphaScale : s.colorScale, alpha);
            };
            const auto colorOps = ops(false);
            const auto alphaOps = ops(true);
            emit(colorOps, alphaOps);
            for (int part = 0; part < 2; ++part)
                if (chains[part].active && chains[part].end == i) chains[part].active = false;
            // Only one previous value survives in the combiner; older register writes are lost.
            const std::uint8_t outputs[2]{static_cast<std::uint8_t>(s.colorReg & 3), static_cast<std::uint8_t>(s.alphaReg & 3)};
            for (int part = 0; part < 2; ++part) {
                written[outputs[part]][part] = true;
                live[part] = outputs[part];
                ranges[outputs[part]][part] = results[part];
                producer[outputs[part]][part] = ready[part] - 1;
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
// Half-scale sums take more stages and constants; one that no longer fits in six, or isn't exact where fewer of them
// (color's or alpha's alone, or none) are, is translated with fewer.
CombinerProgram translate(const ModelMaterial& material, bool leading, bool known, bool color1, bool& wantsLeading) {
    Compiler compiler(material, leading, known, color1);
    const auto program = compiler.run();
    wantsLeading = compiler.wantsLeading;
    if (program.exact || !compiler.fusedAny) return program;
    for (const auto& [color, alpha] : {std::pair{true, false}, std::pair{false, true}, std::pair{false, false}}) {
        Compiler fewer(material, leading, known, color1);
        fewer.fuseAllowed[0] = color, fewer.fuseAllowed[1] = alpha;
        const auto other = fewer.run();
        if (other.exact || (!color && !alpha && compiler.overflowed)) return wantsLeading = fewer.wantsLeading, other;
    }
    return program;
}

CombinerProgram compile(const ModelMaterial& material, bool known, bool color1) {
    bool wantsLeading = false, unused = false;
    const auto program = translate(material, false, known, color1, wantsLeading);
    if (program.exact || !wantsLeading) return program;
    // Stage 0 cannot read the buffer color; spending a stage lets stage 1 read it instead.
    // A clamp between ops is a smaller error than a wrong constant.
    const auto shifted = translate(material, true, known, color1, unused);
    return shifted.exact || std::strcmp(shifted.approximation, "more than two constants") ? shifted : program;
}
void clearDead(ModelMaterial& m);

// The game's water contour bands: P = 2 K0 T0 T1 from two gray textures, then P + (P.r > A0 ? C0 : 0) unclamped, read
// as the weight of lerp(C1, K1, c), whose low eight bits wrap: a bright band where A0 < P < 1 - C0, P shifted down
// above it. Six stages carry P at K0 / 2 scale beside two sharpened steps (at A0 and 1 - C0) and blend to K1 inside
// their window. Approximate: P past K0 / 2 saturates (both texels past one half) and the steps aren't sharp.
bool wrapBand(const ModelMaterial& m, std::uint8_t grayMaps, CombinerProgram& out) {
    if (m.tevStageCount != 4) return false;
    const TevStage* s = m.stages;
    const auto color = [](const TevStage& t, std::initializer_list<int> in, int op, int bias, int scale, int clamp) {
        return std::equal(in.begin(), in.end(), t.colorIn) && t.colorOp == op && t.colorBias == bias && t.colorScale == scale &&
               t.colorClamp == clamp && t.colorReg == 0;
    };
    const auto rasterAlpha = [](const TevStage& t) {
        return t.alphaIn[0] == 5 && t.alphaIn[1] == 7 && t.alphaIn[2] == 7 && t.alphaIn[3] == 7 && !t.alphaOp && !t.alphaBias && !t.alphaScale &&
               t.alphaReg == 0 && t.channel == 4;
    };
    const auto gray = [&](const TevStage& t) {
        if (t.texMap >= 8 || m.textures[t.texMap] < 0 || !(grayMaps >> t.texMap & 1)) return false;
        const std::uint8_t* swap = m.swapTables[t.texSwap & 3];
        return swap[0] < 3 && swap[1] < 3 && swap[2] < 3;
    };
    if (!color(s[0], {15, 14, 8, 15}, 0, 0, 0, 1) || !color(s[1], {15, 0, 8, 15}, 0, 0, 1, 1) || !color(s[2], {0, 3, 2, 0}, 8, 3, 0, 0) ||
        !color(s[3], {4, 14, 0, 15}, 0, 0, 0, 1) || !rasterAlpha(s[2]) || !rasterAlpha(s[3]) || !gray(s[0]) || !gray(s[1]))
        return false;
    const auto& c0 = m.tevColors[0];
    const float k0 = konst(m, s[0].konstColor, 0), a0 = c0[3], k = 256 / 255.0f - c0[0], q = k0 / 2;
    if (konst(m, s[0].konstColor, 1) != k0 || konst(m, s[0].konstColor, 2) != k0 || c0[1] != c0[0] || c0[2] != c0[0]) return false;
    const float a1 = a0 / q - 7 / 128.0f, b1 = k / q - 0.00686f;
    float c1[3], d[3];
    for (int i = 0; i < 3; ++i) c1[i] = wrapByte(m.tevColors[1][i]), d[i] = konst(m, s[3].konstColor, i) - c1[i];
    const auto unit = [](float v) { return v >= 0 && v <= 1; };
    if (!(q > 0) || !(a0 >= 0) || !(a0 < k) || !(k <= 0.5f) || !unit(a1) || !unit(b1) || !unit(d[0]) || !unit(d[1]) || !unit(d[2])) return false;
    CombinerProgram p;
    p.unitCount = 2;
    p.units[0] = {m.textures[s[0].texMap], static_cast<std::uint8_t>(s[0].texCoord < 8 ? s[0].texCoord : 0)};
    p.units[1] = {m.textures[s[1].texMap], static_cast<std::uint8_t>(s[1].texCoord < 8 ? s[1].texCoord : 0)};
    using F = CombineFunc;
    using S = CombineSource;
    using O = CombineOperand;
    const auto stage = [&](int i, F cf, std::uint8_t cs, std::initializer_list<std::pair<S, O>> cin, F af, std::uint8_t as,
                           std::initializer_list<std::pair<S, O>> ain) -> CombineStage& {
        CombineStage& t = p.stages[i];
        t.colorFunc = cf, t.colorScale = cs, t.alphaFunc = af, t.alphaScale = as;
        int n = 0;
        for (const auto& [source, operand] : cin) t.colorSource[n] = source, t.colorOperand[n++] = operand;
        n = 0;
        for (const auto& [source, operand] : ain) t.alphaSource[n] = source, t.alphaOperand[n++] = operand;
        return t;
    };
    // Q = 4 T0 T1 everywhere, so P = q Q.
    stage(0, F::Modulate, 4, {{S::Texture0, O::Red}, {S::Texture1, O::Red}}, F::Modulate, 4, {{S::Texture0, O::Red}, {S::Texture1, O::Red}});
    // Rising ramps toward each step in red and blue; alpha P + 1/2 - k.
    CombineStage& rise = stage(1, F::Subtract, 4, {{S::Previous, O::Color}, {S::Constant, O::Color}}, F::MultiplyAdd, 1,
                               {{S::Previous, O::Alpha}, {S::Constant, O::Alpha}, {S::Buffer, O::Alpha}});
    rise.constant = {a1, 0, b1, q};
    // Red falls from one at A0; blue is k until the step at 1 - C0.
    CombineStage& fall = stage(2, F::Subtract, 4, {{S::Constant, O::Color}, {S::Previous, O::Color}}, F::Replace, 1, {{S::Previous, O::Alpha}});
    fall.constant = {0.25f, 0, k / 4, 0};
    // Doubled thrice: red is 1 - [P > A0], blue k (1 - [P > 1 - C0]); alpha P - k [P > 1 - C0], the wrapped weight.
    stage(3, F::Add, 4, {{S::Previous, O::Color}, {S::Previous, O::Color}}, F::AddSigned, 1, {{S::Previous, O::Alpha}, {S::Previous, O::Blue}});
    // C1 + (K1 - C1) weight; alpha the band, between the steps.
    CombineStage& blend = stage(4, F::MultiplyAdd, 1, {{S::Previous, O::Alpha}, {S::Constant, O::Color}, {S::Buffer, O::Color}}, F::Modulate, 4,
                                {{S::Previous, O::OneMinusRed}, {S::Previous, O::Blue}});
    blend.constant = {d[0], d[1], d[2], 0};
    // K1 inside the band; the raster's alpha, as the last GX stages give.
    CombineStage& band = stage(5, F::Interpolate, 1, {{S::Constant, O::Color}, {S::Previous, O::Color}, {S::Previous, O::Alpha}}, F::Replace, 1,
                               {{S::Primary, O::Alpha}});
    for (int i = 0; i < 3; ++i) band.constant[i] = konst(m, s[3].konstColor, i);
    p.bufferColor = {c1[0], c1[1], c1[2], 0.5f - k};
    p.stageCount = 6;
    p.exact = false;
    p.approximation = "a compare's wrap band";
    out = p;
    return true;
}

// OceanSphere's water lines: P = 2 K0 T0 T1 from two gray textures, C0 added where P's red byte equals A0's (an R8
// equality), then lerp(C1, C2, P): its base color plus (C2 - C1) C0 on the lines. Six stages carry G = 2 T0 T1 in alpha
// beside two edges sharpened on 2G at the line's limits, midpoints on them so a line keeps its width. Approximate: G
// past one saturates (both texels past 0.7) and each edge spreads over about a quarter of a step.
bool equalLine(const ModelMaterial& m, std::uint8_t grayMaps, CombinerProgram& out) {
    if (m.tevStageCount != 4) return false;
    const TevStage* s = m.stages;
    const auto color = [](const TevStage& t, std::initializer_list<int> in, int op, int bias, int scale, int clamp) {
        return std::equal(in.begin(), in.end(), t.colorIn) && t.colorOp == op && t.colorBias == bias && t.colorScale == scale &&
               t.colorClamp == clamp && t.colorReg == 0;
    };
    const auto gray = [&](const TevStage& t) {
        if (t.texMap >= 8 || m.textures[t.texMap] < 0 || !(grayMaps >> t.texMap & 1)) return false;
        const std::uint8_t* swap = m.swapTables[t.texSwap & 3];
        return swap[0] < 3 && swap[1] < 3 && swap[2] < 3;
    };
    const TevStage& last = s[3];
    if (!color(s[0], {15, 8, 14, 15}, 0, 0, 0, 1) || !color(s[1], {15, 8, 0, 15}, 0, 0, 1, 1) || !color(s[2], {0, 3, 2, 0}, 9, 3, 0, 0) ||
        !color(s[3], {4, 6, 0, 15}, 0, 0, 0, 1) || !gray(s[0]) || !gray(s[1]) || last.alphaIn[0] != 2 || last.alphaIn[1] != 7 ||
        last.alphaIn[2] != 7 || last.alphaIn[3] != 7 || last.alphaOp || last.alphaBias || last.alphaScale || last.alphaReg)
        return false;
    const auto& c0 = m.tevColors[0];
    float k0[3], c1[3], d[3], slope[3], line[3];
    for (int i = 0; i < 3; ++i) {
        k0[i] = konst(m, s[0].konstColor, i), c1[i] = wrapByte(m.tevColors[1][i]), d[i] = wrapByte(m.tevColors[2][i]) - c1[i];
        slope[i] = d[i] * k0[i], line[i] = d[i] * c0[i];
    }
    const float a0 = wrapByte(c0[3]), step = 1 / 255.0f, sharp = 1 / 256.0f;
    if (!(k0[0] > 0) || !(a0 >= step)) return false;
    // The line's limits in 2G (P's red byte rounds to A0's), each edge's midpoint on its limit.
    const float rise = 2 * (a0 - step / 2) / k0[0] - sharp / 2, fall = 2 * (a0 + step / 2) / k0[0] - sharp / 2;
    const auto unit = [](float v) { return v >= 0 && v <= 1; };
    for (int i = 0; i < 3; ++i)
        // On a line P + C0 stays under the wrap, so the line only adds (C2 - C1) C0.
        if (!unit(c1[i]) || !unit(d[i]) || !unit(slope[i]) || !unit(line[i]) || k0[i] * (a0 + step) / k0[0] + c0[i] > 1) return false;
    if (!unit(rise) || !unit(fall)) return false;
    CombinerProgram p;
    p.unitCount = 2;
    p.units[0] = {m.textures[s[0].texMap], static_cast<std::uint8_t>(s[0].texCoord < 8 ? s[0].texCoord : 0)};
    p.units[1] = {m.textures[s[1].texMap], static_cast<std::uint8_t>(s[1].texCoord < 8 ? s[1].texCoord : 0)};
    using F = CombineFunc;
    using S = CombineSource;
    using O = CombineOperand;
    const auto stage = [&](int i, F cf, std::uint8_t cs, std::initializer_list<std::pair<S, O>> cin, F af,
                           std::initializer_list<std::pair<S, O>> ain) -> CombineStage& {
        CombineStage& t = p.stages[i];
        t.colorFunc = cf, t.colorScale = cs, t.alphaFunc = af, t.alphaScale = 1;
        int n = 0;
        for (const auto& [source, operand] : cin) t.colorSource[n] = source, t.colorOperand[n++] = operand;
        n = 0;
        for (const auto& [source, operand] : ain) t.alphaSource[n] = source, t.alphaOperand[n++] = operand;
        return t;
    };
    // 2G in color (only the lines read it), G in alpha.
    stage(0, F::Modulate, 4, {{S::Texture0, O::Red}, {S::Texture1, O::Red}}, F::Modulate, {{S::Texture0, O::Red}, {S::Texture1, O::Red}})
        .alphaScale = 2;
    // Red rises at the line's lower limit, blue at its upper one, sharpened 4 x 8 x 8; alpha keeps G.
    stage(1, F::Subtract, 4, {{S::Previous, O::Color}, {S::Constant, O::Color}}, F::Replace, {{S::Previous, O::Alpha}}).constant = {rise, 0, fall, 0};
    stage(2, F::Add, 4, {{S::Previous, O::Color}, {S::Previous, O::Color}}, F::Replace, {{S::Previous, O::Alpha}});
    stage(3, F::Add, 4, {{S::Previous, O::Color}, {S::Previous, O::Color}}, F::Replace, {{S::Previous, O::Alpha}});
    // The base, C1 + (C2 - C1) K0 G; alpha the line, between the edges.
    CombineStage& base = stage(4, F::MultiplyAdd, 1, {{S::Previous, O::Alpha}, {S::Constant, O::Color}, {S::Buffer, O::Color}}, F::Modulate,
                               {{S::Previous, O::Red}, {S::Previous, O::OneMinusBlue}});
    base.constant = {slope[0], slope[1], slope[2], 0};
    // The line's (C2 - C1) C0 added; alpha A1, as the last GX stage gives.
    CombineStage& lines = stage(5, F::MultiplyAdd, 1, {{S::Previous, O::Alpha}, {S::Constant, O::Color}, {S::Previous, O::Color}}, F::Replace,
                                {{S::Constant, O::Alpha}});
    lines.constant = {line[0], line[1], line[2], wrapByte(m.tevColors[1][3])};
    p.bufferColor = {c1[0], c1[1], c1[2], 0};
    p.stageCount = 6;
    p.exact = false;
    p.approximation = "an equality's line";
    out = p;
    return true;
}
} // namespace

CombinerProgram compileTev(const ModelMaterial& material, bool color1FromUnit, std::uint8_t grayMaps) {
    // A register of known value read as a constant frees the previous value but takes a constant slot.
    const auto previous = compile(material, false, color1FromUnit);
    if (previous.exact) return previous;
    const auto constants = compile(material, true, color1FromUnit);
    if (constants.exact) return constants;
    // Stages that only fed a saturated result fixed whatever they give take no combiner stages.
    ModelMaterial folded = material;
    if (Compiler(material, false, false, color1FromUnit).foldFixed(folded)) {
        clearDead(folded);
        for (const bool known : {false, true})
            if (const auto program = compile(folded, known, color1FromUnit); program.exact) return program;
    }
    if (CombinerProgram band; wrapBand(material, grayMaps, band)) return band;
    if (CombinerProgram line; equalLine(material, grayMaps, line)) return line;
    return previous;
}
namespace {
// GX inputs by number: color CPREV 0, APREV 1, C0-C2 2/4/6, A0-A2 3/5/7, TEXC 8, TEXA 9, RASC 10, RASA 11, ONE 12,
// HALF 13, KONST 14, ZERO 15; alpha APREV 0, A0-A2 1-3, TEXA 4, RASA 5, KONST 6, ZERO 7.
constexpr std::uint8_t ColorZero = 15, AlphaZero = 7;

// The stage's color left as its d alone, biased and scaled.
void dOnly(TevStage& s) {
    s.colorIn[0] = s.colorIn[1] = s.colorIn[2] = ColorZero;
    s.colorOp = 0;
}
void passColor(TevStage& s) { dOnly(s), s.colorIn[3] = 0, s.colorBias = s.colorScale = 0; }

// d + a term that reads nothing of PREV, unscaled: a stage the ones before it can be split from.
bool additive(const TevStage& s) {
    return s.colorIn[3] == 0 && s.colorOp == 0 && !s.colorBias && !s.colorScale &&
           std::none_of(s.colorIn, s.colorIn + 3, [](std::uint8_t in) { return in <= 1; });
}

// Stages whose result nothing reads are cleared, so their textures take no unit: each register (PREV, C0-C2 as bits
// 0-3) followed back from what the last stage leaves in PREV.
void clearDead(ModelMaterial& m) {
    unsigned color = 1, alpha = 1;
    for (int i = int(m.tevStageCount) - 1; i >= 0; --i) {
        TevStage& s = m.stages[i];
        const unsigned colorOut = 1u << (s.colorReg & 3), alphaOut = 1u << (s.alphaReg & 3);
        const bool colorLive = color & colorOut, alphaLive = alpha & alphaOut;
        color &= ~colorOut, alpha &= ~alphaOut;
        if (!colorLive) std::fill(s.colorIn, s.colorIn + 4, ColorZero), s.colorOp = s.colorBias = s.colorScale = 0;
        if (!alphaLive) std::fill(s.alphaIn, s.alphaIn + 4, AlphaZero), s.alphaOp = s.alphaBias = s.alphaScale = 0;
        for (const std::uint8_t in : s.colorIn)
            if (in <= 7) (in & 1 ? alpha : color) |= 1u << (in / 2);
        for (const std::uint8_t in : s.alphaIn)
            if (in <= 3) alpha |= 1u << in;
    }
}

// The stage writing the register a color input reads before stage `before`, or -1 for the material's starting value.
int producer(const ModelMaterial& m, unsigned before, std::uint8_t in) {
    for (int i = int(before) - 1; i >= 0; --i)
        if (((in & 1 ? m.stages[i].alphaReg : m.stages[i].colorReg) & 3) == in / 2) return i;
    return -1;
}

// A color input (as stage `before` reads it) that is never negative: only d can be, as a, b and c read 8 bits, and
// a sum or a compare (adding c or nothing) keeps d's sign.
bool nonNegative(const ModelMaterial& m, unsigned before, std::uint8_t in) {
    if (in > 7) return true;
    const int w = producer(m, before, in);
    if (w < 0) {
        const auto& v = m.tevColors[(in / 2 + 3) % 4];
        return in & 1 ? v[3] >= 0 : std::min({v[0], v[1], v[2]}) >= 0;
    }
    const TevStage& s = m.stages[w];
    if (in & 1) {
        if (s.alphaClamp) return true;
        static constexpr std::uint8_t toColor[8]{1, 3, 5, 7, 15, 15, 15, 15};
        return (s.alphaOp == 0 || s.alphaOp >= 8) && s.alphaBias != 2 && nonNegative(m, unsigned(w), toColor[s.alphaIn[3] & 7]);
    }
    return s.colorClamp || ((s.colorOp == 0 || s.colorOp >= 8) && s.colorBias != 2 && nonNegative(m, unsigned(w), s.colorIn[3]));
}

// A color input (as stage `before` reads it) within 0..1: saturated, or a lerp of bytes with nothing added.
bool inUnit(const ModelMaterial& m, unsigned before, std::uint8_t in) {
    if (in > 7) return true;
    const int w = producer(m, before, in);
    if (w < 0) {
        const auto& v = m.tevColors[(in / 2 + 3) % 4];
        const auto unit = [](float x) { return x >= 0 && x <= 1; };
        return in & 1 ? unit(v[3]) : unit(v[0]) && unit(v[1]) && unit(v[2]);
    }
    const TevStage& s = m.stages[w];
    if (in & 1) return s.alphaClamp || (!s.alphaOp && !s.alphaBias && !s.alphaScale && s.alphaIn[3] == AlphaZero);
    return s.colorClamp || (!s.colorOp && !s.colorBias && !s.colorScale && s.colorIn[3] == ColorZero);
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
    pass.material = e, pass.blend = TevPass::AddWhereEqual, pass.reference = std::uint8_t(std::lround(std::clamp(b, 0.0f, 1.0f) * 255));
    return true;
}

// Stage j's term alone (the later stages passing it on), under the whole alpha chain: what the stage added.
TevPass termPass(const ModelMaterial& m, unsigned j, TevPass::Blend blend) {
    TevPass pass;
    pass.material = m, pass.blend = blend;
    pass.material.stages[j].colorIn[3] = ColorZero, pass.material.stages[j].colorBias = 0;
    for (unsigned i = j + 1; i < m.tevStageCount; ++i) passColor(pass.material.stages[i]);
    clearDead(pass.material);
    return pass;
}

bool alwaysShown(const ModelMaterial& m) {
    const auto all = [](std::uint8_t compare, std::uint8_t reference) { return compare == 7 || (compare == 6 && !reference); };
    const auto& t = m.alphaTest;
    const bool first = all(t.compare0, t.reference0), second = all(t.compare1, t.reference1);
    return t.op == 1 ? first || second : t.op == 0 && first && second;
}

// A pass of the last stage's x drawn over the rest by its scalar input `by` (halved when asked) as the pass's alpha.
// The scalar takes alpha's place, so no alpha test; the stages before stay for the registers x and the scalar read.
bool overOf(const ModelMaterial& m, std::uint8_t grayMaps, std::uint8_t x, std::uint8_t by, bool half, TevPass& pass) {
    const unsigned last = m.tevStageCount - 1u;
    const TevStage& s = m.stages[last];
    if (x <= 1 || by <= 1 || !alwaysShown(m)) return false;
    ModelMaterial e = m;
    TevStage& t = e.stages[last];
    std::uint8_t alpha = 0, konst = 0;
    switch (by) {
    case 3: case 5: case 7: alpha = std::uint8_t((by - 1) / 2); break;
    case 9: alpha = 4; break;
    case 11: alpha = 5; break;
    case 12: alpha = 6, konst = 0; break;
    case 13: alpha = 6, konst = 4; break;
    case 14:
        if (s.konstColor >= 0x08 && s.konstColor < 0x10) return false;  // three components
        alpha = 6, konst = s.konstColor;
        break;
    case 8: {
        if (!(grayMaps >> s.texMap & 1)) return false;
        // The texture's alpha read from its red, through a table the stage's raster color doesn't use.
        const std::uint8_t table = s.rasSwap == 1 ? 2 : 1, red = m.swapTables[s.texSwap][0];
        std::copy(m.swapTables[s.texSwap], m.swapTables[s.texSwap] + 3, e.swapTables[table]);
        e.swapTables[table][3] = red, t.texSwap = table, alpha = 4;
        break;
    }
    default: return false;
    }
    if (half && alpha == 6) return false;  // the konst can't hold both
    t.colorIn[0] = t.colorIn[1] = t.colorIn[2] = ColorZero, t.colorIn[3] = x;
    t.colorOp = t.colorBias = t.colorScale = 0, t.colorClamp = 1;
    t.alphaIn[0] = t.alphaIn[2] = AlphaZero, t.alphaIn[1] = t.alphaIn[3] = AlphaZero;
    if (half) t.alphaIn[1] = alpha, t.alphaIn[2] = 6, konst = 4;
    else t.alphaIn[3] = alpha;
    t.alphaOp = t.alphaBias = t.alphaScale = 0, t.alphaClamp = 1, t.alphaReg = 0, t.konstAlpha = konst;
    clearDead(e);
    pass.material = e, pass.blend = TevPass::Over;
    return true;
}

// The last stage's lerp from PREV (within 0..1) to x by a scalar, neither read from PREV: x drawn over the rest by the
// scalar, the first pass leaving PREV.
bool overPass(const ModelMaterial& m, std::uint8_t grayMaps, TevPass& pass) {
    const unsigned last = m.tevStageCount - 1u;
    const TevStage& s = m.stages[last];
    if (s.colorIn[0] != 0 || s.colorIn[3] != ColorZero || s.colorOp || s.colorBias || s.colorScale || (s.colorReg & 3) || !inUnit(m, last, 0))
        return false;
    return overOf(m, grayMaps, s.colorIn[1], s.colorIn[2], false, pass);
}

// A pass of a constant color under the material's alpha chain (so its alpha test too), through a konst whose color no
// alpha stage reads.
bool constantPass(const ModelMaterial& m, TevPass::Blend blend, const float rgb[3], TevPass& pass) {
    int slot = -1;
    for (int k = 0; k < 4 && slot < 0; ++k) {
        bool read = false;
        for (unsigned i = 0; i < m.tevStageCount; ++i) {
            const TevStage& s = m.stages[i];
            read |= std::count(s.alphaIn, s.alphaIn + 4, 6) && s.konstAlpha >= 0x10 && s.konstAlpha < 0x1c && (s.konstAlpha & 3) == k;
        }
        if (!read) slot = k;
    }
    if (slot < 0) return false;
    pass.material = m, pass.blend = blend;
    ModelMaterial& e = pass.material;
    for (int c = 0; c < 3; ++c) e.konstColors[slot][c] = rgb[c];
    TevStage& t = e.stages[m.tevStageCount - 1u];
    t.colorIn[0] = t.colorIn[1] = t.colorIn[2] = ColorZero, t.colorIn[3] = 14;
    t.colorOp = t.colorBias = t.colorScale = 0, t.colorClamp = 1, t.konstColor = std::uint8_t(0x0c + slot);
    clearDead(e);
    return true;
}

// A color input that no input changes: a constant, the konst, or a register no stage before writes.
bool fixedInput(const ModelMaterial& m, unsigned stage, std::uint8_t in, float v[3]) {
    for (int c = 0; c < 3; ++c) {
        if (in == ColorZero) v[c] = 0;
        else if (in == 12) v[c] = 1;
        else if (in == 13) v[c] = 0.5f;
        else if (in == 14) v[c] = konst(m, m.stages[stage].konstColor, c);
        else if (in <= 7 && in > 1 && producer(m, stage, in) < 0) v[c] = m.tevColors[(in / 2 + 3) % 4][in & 1 ? 3 : c];
        else return false;
    }
    return true;
}

// The last stage as passes finishing PREV (within 0..1, as the framebuffer holds it), the first pass leaving it. An
// affine g PREV + h is Sub (-h / g where h < 0), Scale and Gain (g), then Add (h where h > 0), saturating as GX does
// once. PREV plus a lerp from PREV to x by t is first x drawn over by t / 2, then the same with g = 2. A sum with a
// -1/2 bias is drawn halved without it (never past one), then Sub 1/4 and a Gain for each doubling to its scale.
bool finishPasses(const ModelMaterial& m, std::uint8_t grayMaps, ModelMaterial& first, std::vector<TevPass>& out) {
    const unsigned last = m.tevStageCount - 1u;
    const TevStage& s = m.stages[last];
    if ((s.colorReg & 3) || s.colorOp > 1 || s.colorBias == 3 || !s.colorClamp || !inUnit(m, last, 0)) return false;
    const float scale = s.colorScale == 1 ? 2 : s.colorScale == 2 ? 4 : s.colorScale == 3 ? 0.5f : 1;
    const float bias = s.colorBias == 1 ? 0.5f : s.colorBias == 2 ? -0.5f : 0;
    const std::uint8_t* in = s.colorIn;
    bool prev[4], fixed[4];
    float v[4][3];
    for (int j = 0; j < 4; ++j) prev[j] = in[j] == 0, fixed[j] = !prev[j] && fixedInput(m, last, in[j], v[j]);
    float g[3], h[3];
    out.clear();
    const bool affine = (prev[0] || fixed[0]) && (prev[1] || fixed[1]) && (prev[2] || fixed[2]) && (prev[3] || fixed[3]) &&
                        (prev[0] || prev[1] || prev[2] || prev[3]) && !(prev[2] && (prev[0] || prev[1]));
    if (affine) {
        for (int c = 0; c < 3; ++c) {
            const auto f = [&](float p) {
                const auto at = [&](int j) { return prev[j] ? p : j < 3 ? wrapByte(v[j][c]) : v[j][c]; };
                const float lerp = at(0) * (1 - at(2)) + at(1) * at(2);
                return scale * (at(3) + (s.colorOp ? -lerp : lerp) + bias);
            };
            h[c] = f(0), g[c] = f(1) - h[c];
        }
    } else if (!s.colorOp && prev[0] && prev[3] && !prev[1] && !prev[2] && !fixed[2]) {
        // P + P (1 - t) + x t + bias = 2 (P (1 - t / 2) + x t / 2) + bias
        TevPass over;
        if (!overOf(m, grayMaps, in[1], in[2], true, over)) return false;
        out.push_back(over);
        for (int c = 0; c < 3; ++c) g[c] = 2 * scale, h[c] = scale * bias;
    } else if (!s.colorOp && s.colorBias == 2 && s.colorScale != 2 && inUnit(m, last, in[3])) {
        first.stages[last].colorBias = 0, first.stages[last].colorScale = 3;
        const float quarter[3]{0.25f, 0.25f, 0.25f}, one[3]{1, 1, 1};
        TevPass pass;
        if (!constantPass(m, TevPass::Sub, quarter, pass)) return false;
        out.push_back(pass);
        for (int k = s.colorScale == 3 ? 0 : s.colorScale == 0 ? 1 : 2; k > 0; --k) {
            if (!constantPass(m, TevPass::Gain, one, pass)) return false;
            out.push_back(pass);
        }
        return true;
    } else {
        return false;
    }
    float sub[3], times[3], gain[3], add[3];
    bool anySub = false, anyScale = false, anyGain = false, anyAdd = false;
    for (int c = 0; c < 3; ++c) {
        if (g[c] <= 0 || g[c] > 2 || -h[c] > g[c]) return false;
        sub[c] = std::max(-h[c], 0.0f) / g[c], times[c] = std::min(g[c], 1.0f), gain[c] = std::max(g[c], 1.0f) - 1, add[c] = std::min(std::max(h[c], 0.0f), 1.0f);
        anySub |= sub[c] > 0, anyScale |= times[c] < 1, anyGain |= gain[c] > 0, anyAdd |= add[c] > 0;
    }
    for (const auto& [any, blend, rgb] : {std::tuple{anySub, TevPass::Sub, sub}, std::tuple{anyScale, TevPass::Scale, times},
                                          std::tuple{anyGain, TevPass::Gain, gain}, std::tuple{anyAdd, TevPass::Add, add}}) {
        TevPass pass;
        if (!any) continue;
        if (!constantPass(m, blend, rgb, pass)) return false;
        out.push_back(pass);
    }
    // The first pass ends before the last stage when that stage's alpha only passes PREV's on, or when no alpha test
    // reads it: then the last pass writes the material's alpha.
    const bool alphaPasses = !s.alphaOp && !s.alphaBias && !s.alphaScale && !(s.alphaReg & 3) &&
                             ((s.alphaIn[0] == AlphaZero && s.alphaIn[2] == AlphaZero && s.alphaIn[3] == 0) ||
                              (s.alphaIn[0] == 0 && s.alphaIn[2] == AlphaZero && s.alphaIn[3] == AlphaZero)) &&
                             (!s.alphaClamp || inUnit(m, last, 1));
    if (out.empty()) return false;
    if (alphaPasses || (alwaysShown(m) && out.back().blend != TevPass::Over)) --first.tevStageCount, out.back().writesAlpha = !alphaPasses;
    else passColor(first.stages[last]);
    return true;
}
} // namespace

bool splitTev(const ModelMaterial& m, TevPasses& out, std::uint8_t grayMaps) {
    const bool overAlpha = m.blend.type == 1 && m.blend.source == 4;  // GX_BM_BLEND from GX_BL_SRCALPHA
    const bool opaque = m.blend.type == 0 || (m.blend.type == 1 && m.blend.source == 1 && m.blend.destination == 0);
    if ((!overAlpha && !opaque) || !m.tevStageCount) return false;
    int k = -1;
    for (unsigned i = 0; i < m.tevStageCount && k < 0; ++i)
        if (m.stages[i].colorOp == 9) k = int(i);
    out.first = m, out.extra.clear();
    if (k >= 0) {
        for (unsigned i = 0; i < m.tevStageCount; ++i) {
            const TevStage& s = m.stages[i];
            if (!overAlpha || (s.colorReg & 3) || (s.alphaReg & 3) || (i != unsigned(k) && (s.colorOp > 1 || s.alphaOp > 1))) return false;
        }
        const TevStage& s = m.stages[k];
        if (s.colorIn[0] || s.colorIn[3] || s.colorBias || s.colorScale || !passesAllShown(m)) return false;
        for (unsigned i = unsigned(k) + 1; i < m.tevStageCount; ++i)
            if (!additive(m.stages[i])) return false;
        TevPass equal;
        if (!equalityPass(m, unsigned(k), equal)) return false;
        passColor(out.first.stages[k]);
        out.extra.push_back(equal);
    }
    TevPass over;
    const bool overLast = k < 0 && opaque && overPass(m, grayMaps, over);
    const unsigned end = m.tevStageCount - overLast;  // the stages a term may be split from
    if (overLast) passColor(out.first.stages[end]);
    clearDead(out.first);
    // A term splits from a sum into PREV whose d (and bias) is never negative, the stages after it adding terms of their
    // own; added by the blend, saturating, the passes' sum is GX's when the sum saturates too.
    const bool saturates = !opaque || m.stages[end - 1].colorClamp;
    for (int j = int(end) - 1; j > k && saturates && !compileTev(out.first, false, grayMaps).exact; --j) {
        const TevStage& s = m.stages[j];
        if ((s.colorReg & 3) || s.colorOp || s.colorBias == 2 || !nonNegative(m, unsigned(j), s.colorIn[3])) break;
        if (unsigned(j) == end - 1 && s.colorIn[3] == ColorZero) break;  // the first pass would keep nothing
        const bool sampled = std::any_of(s.colorIn, s.colorIn + 3, [](std::uint8_t in) { return in == 8 || in == 9; });
        if (sampled || unitsRead(out.first) <= CombinerProgram::MaxUnits) {
            out.extra.push_back(termPass(m, unsigned(j), opaque ? TevPass::Add : TevPass::AddTimesAlpha));
            dOnly(out.first.stages[j]);
            clearDead(out.first);
        }
        if (!additive(s)) break;
    }
    if (overLast) out.extra.push_back(over);
    if (k < 0 && opaque && !overLast && !compileTev(out.first, false, grayMaps).exact) {
        TevPasses finished;
        finished.first = m;
        if (finishPasses(m, grayMaps, finished.first, finished.extra)) clearDead(finished.first), out = std::move(finished);
    }
    if (out.extra.empty() || unitsRead(out.first) > CombinerProgram::MaxUnits) return false;
    for (const TevPass& pass : out.extra)
        if (unitsRead(pass.material) > CombinerProgram::MaxUnits) return false;
    return true;
}
} // namespace smg
