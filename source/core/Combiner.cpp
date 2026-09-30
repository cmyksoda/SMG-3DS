#include <smg/Combiner.hpp>
#include <algorithm>
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

    explicit Compiler(const ModelMaterial& material) : m(material) {
        registers[0] = m.tevColors[3];
        for (int i = 0; i < 3; ++i) registers[i + 1] = m.tevColors[i];
        for (auto& row : unitFor) for (int& unit : row) unit = -1;
    }

    Arg reg(int index, bool alphaPart, bool colorInput) {
        const int part = alphaPart ? 1 : 0;
        const CombineOperand operand = alphaPart ? CombineOperand::Alpha : CombineOperand::Color;
        if (!written[index][part]) {
            const auto& r = registers[index];
            if (!colorInput) return constant(std::clamp(r[3], 0.0f, 1.0f));
            if (alphaPart) return constant(std::clamp(r[3], 0.0f, 1.0f));
            return constant(std::clamp(r[0], 0.0f, 1.0f), std::clamp(r[1], 0.0f, 1.0f), std::clamp(r[2], 0.0f, 1.0f));
        }
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

    Arg colorInput(const TevStage& s, std::uint8_t input) {
        switch (input) {
        case 0: case 2: case 4: case 6: return reg(input / 2, false, true);
        case 1: case 3: case 5: case 7: return reg(input / 2, true, true);
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

    Arg alphaInput(const TevStage& s, std::uint8_t input) {
        switch (input) {
        case 0: case 1: case 2: case 3: return reg(input, true, false);
        case 4: return texture(s, true, false);
        case 5: return raster(s, true, false);
        case 6: return constant(konst(m, s.konstAlpha, 3));
        default: return kind(Arg::Zero);
        }
    }

    // out = (d +/- lerp(a, b, c) + bias) * scale, as a short list of combiner ops.
    std::vector<Op> build(Arg a, Arg b, Arg c, Arg d, std::uint8_t op, std::uint8_t bias, std::uint8_t scale, bool alpha) {
        std::vector<Op> ops;
        const Arg temp = kind(Arg::Temp, alpha ? CombineOperand::Alpha : CombineOperand::Color);
        const bool subtract = op == 1;
        if (op > 1) approximate("compare op"); // compare modes
        enum { None, Single, Product, Lerp } form;
        Arg x, y;
        if (zero(c) || (a == b)) { form = Single; x = a; }
        else if (one(c)) { form = Single; x = b; }
        else if (zero(a) && zero(b)) { form = None; }
        else if (zero(a)) { form = Product; x = b; y = c; }
        else if (zero(b)) { form = Product; x = a; y = invert(c); }
        else form = Lerp;
        if (form == Single && zero(x)) form = None;

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
        } else if (!subtract && usesPrev(d)) {
            // d + a(1 - c) + bc keeps the incoming previous value in the first op only.
            if (usesPrev(a) || usesPrev(b) || usesPrev(c)) approximate("previous value needed twice");
            ops.push_back({CombineFunc::MultiplyAdd, {a, invert(c), d}});
            ops.push_back({CombineFunc::MultiplyAdd, {b, c, temp}});
        } else {
            if (usesPrev(d)) approximate("previous value needed twice");
            if (form == Product) ops.push_back({CombineFunc::Modulate, {x, y}});
            else ops.push_back({CombineFunc::Interpolate, {b, a, c}});
            ops.push_back({subtract ? CombineFunc::Subtract : CombineFunc::Add, {d, temp}});
        }

        // GX saturates once at the end; a separate op after a sum that passed 1 sees it clipped.
        const auto overflows = [](const Op& op) { return op.func == CombineFunc::Add || op.func == CombineFunc::MultiplyAdd; };
        auto& last = ops.back();
        if (bias == 1) {
            // d - x + 1/2 is d + (1 - x) - 1/2, one signed add without clamping in between.
            if (last.func == CombineFunc::Replace) last = {CombineFunc::Add, {last.args[0], constant(0.5f)}};
            else if (last.func == CombineFunc::Subtract) last = {CombineFunc::AddSigned, {last.args[0], invert(last.args[1])}};
            else {
                if (last.func == CombineFunc::AddSigned) approximate("clamp between ops");
                ops.push_back({CombineFunc::Add, {temp, constant(0.5f)}});
            }
        } else if (bias == 2) {
            if (last.func == CombineFunc::Add) last.func = CombineFunc::AddSigned;
            else if (last.func == CombineFunc::Replace) last = {CombineFunc::AddSigned, {last.args[0], kind(Arg::Zero)}};
            else {
                if (overflows(last)) approximate("clamp between ops");
                ops.push_back({CombineFunc::Subtract, {temp, constant(0.5f)}});
            }
        } else if (bias == 3) {
            approximate("compare bias");
        }
        if (scale == 1 || scale == 2) {
            ops.back().scale = scale == 1 ? 2 : 4;
        } else if (scale == 3) {
            if (overflows(ops.back()) || ops.back().func == CombineFunc::AddSigned) approximate("clamp between ops");
            ops.push_back({CombineFunc::Modulate, {temp, constant(0.5f)}});
        }
        return ops;
    }

    struct Slots { bool used = false; float value[3]{}; };

    // Maps an op argument to a combiner source, placing constants in the stage slot or the buffer.
    void place(const Arg& arg, bool alpha, bool firstOfStage, CombineStage& stage, Slots& slot, Slots& buffer,
               CombineSource& source, CombineOperand& operand) {
        operand = arg.operand;
        switch (arg.kind) {
        case Arg::Prev:
            if (!firstOfStage) approximate("previous value needed twice");
            source = CombineSource::Previous;
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
            if (buffer.used && !matches(buffer)) approximate("more than two constants");
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
        for (std::size_t i = 0; i < count; ++i) {
            if (program.stageCount == CombinerProgram::MaxStages) { approximate("more than six stages"); return; }
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
                place(color.args[k], false, i == 0, stage, colorSlot, colorBuffer, stage.colorSource[k], stage.colorOperand[k]);
                place(alpha.args[k], true, i == 0, stage, alphaSlot, alphaBuffer, stage.alphaSource[k], stage.alphaOperand[k]);
            }
        }
    }

    CombinerProgram run() {
        for (unsigned i = 0; i < m.tevStageCount; ++i) {
            const auto& s = m.stages[i];
            const auto colorOps = build(colorInput(s, s.colorIn[0]), colorInput(s, s.colorIn[1]), colorInput(s, s.colorIn[2]),
                                        colorInput(s, s.colorIn[3]), s.colorOp, s.colorBias, s.colorScale, false);
            const auto alphaOps = build(alphaInput(s, s.alphaIn[0]), alphaInput(s, s.alphaIn[1]), alphaInput(s, s.alphaIn[2]),
                                        alphaInput(s, s.alphaIn[3]), s.alphaOp, s.alphaBias, s.alphaScale, true);
            emit(colorOps, alphaOps);
            // Only one previous value survives in the combiner; older register writes are lost.
            const std::uint8_t outputs[2]{static_cast<std::uint8_t>(s.colorReg & 3), static_cast<std::uint8_t>(s.alphaReg & 3)};
            for (int part = 0; part < 2; ++part) {
                written[outputs[part]][part] = true;
                live[part] = outputs[part];
            }
        }
        if (live[0] != 0 || live[1] != 0) approximate("output left in a register");
        if (!program.stageCount) {
            program.stageCount = 1;
            program.stages[0].colorSource[0] = program.stages[0].alphaSource[0] = CombineSource::Primary;
        }
        return program;
    }
};
} // namespace

CombinerProgram compileTev(const ModelMaterial& material) {
    return Compiler(material).run();
}
} // namespace smg
