#include <smg/Combiner.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
using Rgba = std::array<float, 4>;

struct Inputs {
    Rgba texture[8]; // by model texture index
    Rgba raster;
};

float konst(const smg::ModelMaterial& m, std::uint8_t select, int component) {
    if (select < 8) return (8 - select) / 8.0f;
    if (select >= 0x0c && select < 0x10) return m.konstColors[select - 0x0c][component];
    if (select >= 0x10 && select < 0x20) return m.konstColors[select & 3][(select - 0x10) >> 2];
    return 1;
}

// The GX TEV as documented: every stage computes d +/- lerp(a, b, c), then bias, scale and clamp.
Rgba gx(const smg::ModelMaterial& m, const Inputs& in) {
    Rgba reg[4]{m.tevColors[3], m.tevColors[0], m.tevColors[1], m.tevColors[2]};
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        const auto& s = m.stages[i];
        const bool hasTexture = s.texMap < 8 && m.textures[s.texMap] >= 0;
        const Rgba tex = hasTexture ? in.texture[m.textures[s.texMap]] : Rgba{1, 1, 1, 1};
        const Rgba ras = s.channel == 0xff || s.channel == 6 ? Rgba{} : in.raster;
        const auto colorIn = [&](int input, int c) -> float {
            switch (input) {
            case 0: case 2: case 4: case 6: return reg[input / 2][c];
            case 1: case 3: case 5: case 7: return reg[input / 2][3];
            case 8: return tex[c];
            case 9: return tex[3];
            case 10: return ras[c];
            case 11: return ras[3];
            case 12: return 1;
            case 13: return 0.5f;
            case 14: return konst(m, s.konstColor, c);
            default: return 0;
            }
        };
        const auto alphaIn = [&](int input) -> float {
            switch (input) {
            case 0: case 1: case 2: case 3: return reg[input][3];
            case 4: return tex[3];
            case 5: return ras[3];
            case 6: return konst(m, s.konstAlpha, 3);
            default: return 0;
            }
        };
        const auto combine = [](float a, float b, float c, float d, int op, int bias, int scale) {
            const float lerp = a * (1 - c) + b * c;
            float v = op == 1 ? d - lerp : d + lerp;
            v += bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0;
            v *= scale == 1 ? 2 : scale == 2 ? 4 : scale == 3 ? 0.5f : 1;
            return std::clamp(v, 0.0f, 1.0f);
        };
        Rgba out;
        for (int c = 0; c < 3; ++c)
            out[c] = combine(colorIn(s.colorIn[0], c), colorIn(s.colorIn[1], c), colorIn(s.colorIn[2], c),
                             colorIn(s.colorIn[3], c), s.colorOp, s.colorBias, s.colorScale);
        out[3] = combine(alphaIn(s.alphaIn[0]), alphaIn(s.alphaIn[1]), alphaIn(s.alphaIn[2]), alphaIn(s.alphaIn[3]),
                         s.alphaOp, s.alphaBias, s.alphaScale);
        for (int c = 0; c < 3; ++c) reg[s.colorReg & 3][c] = out[c];
        reg[s.alphaReg & 3][3] = out[3];
    }
    return reg[0];
}

// The PICA200 combiner: every stage saturates, and one constant plus the buffer are available.
Rgba pica(const smg::CombinerProgram& p, const Inputs& in) {
    using smg::CombineOperand;
    using smg::CombineSource;
    // As Citra models it: stage 0 reads zero, and an update shows up two stages later.
    Rgba previous{}, buffer{}, next = p.bufferColor;
    for (unsigned i = 0; i < p.stageCount; ++i) {
        const auto& s = p.stages[i];
        const auto source = [&](CombineSource source) -> Rgba {
            switch (source) {
            case CombineSource::Primary: return in.raster;
            case CombineSource::Texture0: case CombineSource::Texture1: case CombineSource::Texture2:
                return in.texture[p.units[static_cast<int>(source) - static_cast<int>(CombineSource::Texture0)].texture];
            case CombineSource::Constant: return s.constant;
            case CombineSource::Buffer: return buffer;
            default: return previous;
            }
        };
        const auto operand = [](const Rgba& v, CombineOperand op, int c) {
            const int n = static_cast<int>(op);
            const int channel = n < 2 ? c : n < 4 ? 3 : (n - 4) / 2;
            return n & 1 ? 1 - v[channel] : v[channel];
        };
        const auto apply = [](smg::CombineFunc f, float a, float b, float c) {
            switch (f) {
            case smg::CombineFunc::Replace: return a;
            case smg::CombineFunc::Modulate: return a * b;
            case smg::CombineFunc::Add: return a + b;
            case smg::CombineFunc::AddSigned: return a + b - 0.5f;
            case smg::CombineFunc::Interpolate: return a * c + b * (1 - c);
            case smg::CombineFunc::Subtract: return a - b;
            case smg::CombineFunc::MultiplyAdd: return a * b + c;
            }
            return a;
        };
        Rgba out;
        for (int c = 0; c < 3; ++c) {
            float v[3];
            for (int k = 0; k < 3; ++k) v[k] = operand(source(s.colorSource[k]), s.colorOperand[k], c);
            out[c] = std::clamp(apply(s.colorFunc, v[0], v[1], v[2]) * s.colorScale, 0.0f, 1.0f);
        }
        float v[3];
        for (int k = 0; k < 3; ++k) {
            // Alpha operands name alpha, red, green, blue in that order.
            const int n = static_cast<int>(s.alphaOperand[k]);
            const Rgba x = source(s.alphaSource[k]);
            const int channel = n < 4 ? 3 : (n - 4) / 2;
            v[k] = n & 1 ? 1 - x[channel] : x[channel];
        }
        out[3] = std::clamp(apply(s.alphaFunc, v[0], v[1], v[2]) * s.alphaScale, 0.0f, 1.0f);
        previous = out;
        buffer = next;
        if (i < 4 && (p.bufferUpdate[0] & (1 << i))) for (int c = 0; c < 3; ++c) next[c] = out[c];
        if (i < 4 && (p.bufferUpdate[1] & (1 << i))) next[3] = out[3];
    }
    return previous;
}

void dump(const smg::ModelMaterial& m, const smg::CombinerProgram& p, const Inputs& in) {
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        const auto& s = m.stages[i];
        std::fprintf(stderr, "gx %u: map %u chan %u C %u %u %u %u op %u bias %u scale %u | A %u %u %u %u op %u bias %u scale %u | k %02x/%02x\n",
            i, s.texMap, s.channel, s.colorIn[0], s.colorIn[1], s.colorIn[2], s.colorIn[3], s.colorOp, s.colorBias,
            s.colorScale, s.alphaIn[0], s.alphaIn[1], s.alphaIn[2], s.alphaIn[3], s.alphaOp, s.alphaBias, s.alphaScale,
            s.konstColor, s.konstAlpha);
    }
    for (unsigned i = 0; i < p.stageCount; ++i) {
        const auto& s = p.stages[i];
        std::fprintf(stderr, "pica %u: C f%d s%d %d/%d %d/%d %d/%d x%u | A f%d %d/%d %d/%d %d/%d x%u | k %.2f %.2f %.2f %.2f\n", i,
            int(s.colorFunc), 0, int(s.colorSource[0]), int(s.colorOperand[0]), int(s.colorSource[1]), int(s.colorOperand[1]),
            int(s.colorSource[2]), int(s.colorOperand[2]), s.colorScale, int(s.alphaFunc), int(s.alphaSource[0]),
            int(s.alphaOperand[0]), int(s.alphaSource[1]), int(s.alphaOperand[1]), int(s.alphaSource[2]),
            int(s.alphaOperand[2]), s.alphaScale, s.constant[0], s.constant[1], s.constant[2], s.constant[3]);
    }
    const auto g = gx(m, in), q = pica(p, in);
    std::fprintf(stderr, "gx %.3f %.3f %.3f %.3f pica %.3f %.3f %.3f %.3f\n", g[0], g[1], g[2], g[3], q[0], q[1], q[2], q[3]);
}

bool same(const Rgba& a, const Rgba& b) {
    for (int c = 0; c < 4; ++c) if (std::fabs(a[c] - b[c]) > 1e-4f) return false;
    return true;
}

Inputs randomInputs(std::mt19937& rng) {
    std::uniform_real_distribution<float> unit(0, 1);
    Inputs in;
    for (auto& t : in.texture) for (auto& c : t) c = unit(rng);
    for (auto& c : in.raster) c = unit(rng);
    return in;
}

// The Good Egg bean planet's face material, as the disc stores it.
smg::ModelMaterial beanFace() {
    smg::ModelMaterial m;
    m.textures[0] = 1;
    m.textures[1] = 2;
    m.tevColors[0] = {0.96f, 0.94f, 0.51f, 0};
    m.tevColors[1] = {0, 0, 0, 1.44f};
    m.tevColors[2] = {0, 0, 0, 1};
    m.konstColors[0] = {0.09f, 0.21f, 0.16f, 0.46f};
    m.konstColors[1] = {0.42f, 0.63f, 0.24f, 1};
    m.tevStageCount = 3;
    const std::uint8_t color[3][4]{{14, 2, 10, 15}, {15, 0, 8, 1}, {15, 10, 8, 0}};
    const std::uint8_t alpha[3][4]{{6, 7, 5, 7}, {7, 7, 7, 7}, {5, 7, 7, 7}};
    for (int i = 0; i < 3; ++i) {
        auto& s = m.stages[i];
        std::copy(color[i], color[i] + 4, s.colorIn);
        std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.channel = 4;
        s.texMap = s.texCoord = i == 0 ? 0xff : i - 1;
    }
    m.stages[0].alphaScale = 1;
    m.stages[1].konstAlpha = 0x05;
    m.stages[2].konstColor = 0x0d;
    return m;
}

void realMaterial(std::mt19937& rng) {
    const auto material = beanFace();
    const auto program = smg::compileTev(material);
    check(program.exact && program.stageCount == 4, "bean planet face needs a leading stage to read two constants");
    check(program.unitCount == 2 && program.units[0].texture == 1 && program.units[1].texCoord == 1, "texture units in first-use order");
    for (int i = 0; i < 200; ++i) {
        const auto in = randomInputs(rng);
        check(same(gx(material, in), pica(program, in)), "bean planet face matches GX");
    }
}

// d + lerp(a, b, c) with the previous value as both d and c needs it after the first op.
void previousTwice(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0;
    m.textures[1] = 1;
    m.tevStageCount = 2;
    auto& first = m.stages[0];
    std::uint8_t firstColor[4]{15, 8, 10, 15}, firstAlpha[4]{7, 4, 5, 7};
    std::copy(firstColor, firstColor + 4, first.colorIn);
    std::copy(firstAlpha, firstAlpha + 4, first.alphaIn);
    first.texMap = first.texCoord = 0;
    first.channel = 4;
    auto& second = m.stages[1];
    std::uint8_t secondColor[4]{8, 10, 1, 0}, secondAlpha[4]{7, 4, 0, 0};
    std::copy(secondColor, secondColor + 4, second.colorIn);
    std::copy(secondAlpha, secondAlpha + 4, second.alphaIn);
    second.texMap = second.texCoord = 1;
    second.channel = 4;
    const auto program = smg::compileTev(m);
    if (!program.exact || program.bufferUpdate[0] != 0 || program.bufferUpdate[1] != 1) {
        std::fprintf(stderr, "exact %d (%s) updates %u %u\n", program.exact, program.approximation ? program.approximation : "",
                     program.bufferUpdate[0], program.bufferUpdate[1]);
        dump(m, program, randomInputs(rng));
    }
    check(program.exact && program.bufferUpdate[0] == 0 && program.bufferUpdate[1] == 1, "previous alpha kept in the buffer");
    for (int i = 0; i < 200; ++i) {
        const auto in = randomInputs(rng);
        check(same(gx(m, in), pica(program, in)), "buffered previous value matches GX");
    }
}

void randomMaterials(std::mt19937& rng) {
    std::uniform_int_distribution<int> colorInput(0, 15), alphaInput(0, 7), small(0, 3), stageCount(1, 4), selection(0, 0x1f);
    std::uniform_real_distribution<float> unit(0, 1);
    unsigned exact = 0;
    constexpr unsigned trials = 4000;
    for (unsigned t = 0; t < trials; ++t) {
        smg::ModelMaterial m;
        for (int i = 0; i < 4; ++i) {
            m.textures[i] = i;
            for (int c = 0; c < 4; ++c) { m.tevColors[i][c] = unit(rng); m.konstColors[i][c] = unit(rng); }
        }
        m.tevStageCount = stageCount(rng);
        for (unsigned i = 0; i < m.tevStageCount; ++i) {
            auto& s = m.stages[i];
            for (int k = 0; k < 4; ++k) {
                s.colorIn[k] = colorInput(rng);
                s.alphaIn[k] = alphaInput(rng);
            }
            s.colorOp = small(rng) == 0;
            s.alphaOp = small(rng) == 0;
            s.colorBias = small(rng) % 3;
            s.alphaBias = small(rng) % 3;
            s.colorScale = small(rng);
            s.alphaScale = small(rng);
            s.texMap = small(rng);
            s.texCoord = small(rng);
            s.channel = small(rng) ? 4 : 0xff;
            s.konstColor = selection(rng);
            s.konstAlpha = selection(rng) | 0x10;
        }
        const auto program = smg::compileTev(m);
        check(program.stageCount >= 1 && program.stageCount <= 6 && program.unitCount <= 3, "program fits the hardware");
        if (!program.exact) continue;
        ++exact;
        for (int i = 0; i < 20; ++i) {
            const auto in = randomInputs(rng);
            if (same(gx(m, in), pica(program, in))) continue;
            std::fprintf(stderr, "material %u\n", t);
            dump(m, program, in);
            check(false, "exact program matches GX");
        }
    }
    std::printf("%u of %u random materials translated exactly\n", exact, trials);
    check(exact > trials / 50, "random materials exercise the exact path");
}
} // namespace

int runCombinerTests() {
    std::mt19937 rng(1234);
    realMaterial(rng);
    previousTwice(rng);
    randomMaterials(rng);
    std::printf("%u TEV combiner checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runCombinerTests(); }
#endif
