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

// Inputs a, b and c take a register's low eight bits, and so does the output: modulo 256 steps, kept
// continuous so it compares with the combiner's unrounded arithmetic.
float wrapByte(float v) {
    if (v >= 0 && v <= 1) return v;
    return v - 256 / 255.0f * std::floor(v * 255 / 256 + 1e-6f);
}

// The GX TEV as Dolphin models it: every stage computes d +/- lerp(a, b, c), then bias and scale; it
// saturates only when its clamp bit is set (else it keeps -1024..1023), and the output is taken modulo 256.
Rgba gx(const smg::ModelMaterial& m, const Inputs& in) {
    Rgba reg[4]{m.tevColors[3], m.tevColors[0], m.tevColors[1], m.tevColors[2]};
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        const auto& s = m.stages[i];
        const bool hasTexture = s.texMap < 8 && m.textures[s.texMap] >= 0;
        const auto swap = [&](const Rgba& v, std::uint8_t table) {
            const std::uint8_t* t = m.swapTables[table];
            return Rgba{v[t[0]], v[t[1]], v[t[2]], v[t[3]]};
        };
        const Rgba tex = swap(hasTexture ? in.texture[m.textures[s.texMap]] : Rgba{1, 1, 1, 1}, s.texSwap);
        const Rgba ras = swap(s.channel == 0xff || s.channel == 6 ? Rgba{} : in.raster, s.rasSwap);
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
        // A compare weighs a against b by component, or for R8 by their reds (redA, redB).
        const auto combine = [](float a, float b, float c, float d, int op, int bias, int scale, bool clamp, float redA, float redB) {
            a = wrapByte(a), b = wrapByte(b), c = wrapByte(c);
            const float lerp = a * (1 - c) + b * c;
            float v = op == 1 ? d - lerp : d + lerp;
            v += bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0;
            v *= scale == 1 ? 2 : scale == 2 ? 4 : scale == 3 ? 0.5f : 1;
            if (op == 8 || op == 9) a = wrapByte(redA), b = wrapByte(redB);
            // Equality is of the bytes the TEV holds.
            if (op >= 8) v = d + ((op & 1 ? std::lround(a * 255) == std::lround(b * 255) : a > b) ? c : 0);  // no GR16 or BGR24
            return clamp ? std::clamp(v, 0.0f, 1.0f) : std::clamp(v, -1024 / 255.0f, 1023 / 255.0f);
        };
        Rgba out;
        for (int c = 0; c < 3; ++c)
            out[c] = combine(colorIn(s.colorIn[0], c), colorIn(s.colorIn[1], c), colorIn(s.colorIn[2], c),
                             colorIn(s.colorIn[3], c), s.colorOp, s.colorBias, s.colorScale, s.colorClamp,
                             colorIn(s.colorIn[0], 0), colorIn(s.colorIn[1], 0));
        const float a = alphaIn(s.alphaIn[0]), b = alphaIn(s.alphaIn[1]);
        out[3] = combine(a, b, alphaIn(s.alphaIn[2]), alphaIn(s.alphaIn[3]), s.alphaOp, s.alphaBias, s.alphaScale, s.alphaClamp, a, b);
        for (int c = 0; c < 3; ++c) reg[s.colorReg & 3][c] = out[c];
        reg[s.alphaReg & 3][3] = out[3];
    }
    Rgba out = reg[0];
    for (auto& c : out) c = wrapByte(c);
    return out;
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
    for (int r = 0; r < 4; ++r)
        std::fprintf(stderr, "tev %d: %.3f %.3f %.3f %.3f\n", r, m.tevColors[r][0], m.tevColors[r][1], m.tevColors[r][2], m.tevColors[r][3]);
    for (unsigned i = 0; i < m.tevStageCount; ++i)
        std::fprintf(stderr, "clamp %u: %u %u, reg %u %u\n", i, m.stages[i].colorClamp, m.stages[i].alphaClamp, m.stages[i].colorReg, m.stages[i].alphaReg);
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

// Good Egg's starting planet grass: C0 alpha is -0.5, which a register read as d keeps. Clamped to zero,
// the alpha stage stayed above 0.5 and stage 1 added it to every color, painting the planet white.
void diskGardenGrass(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0;
    m.tevColors[0] = {72 / 255.0f, 56 / 255.0f, 13 / 255.0f, -128 / 255.0f};
    m.tevColors[1] = {0, 0, 0, 367 / 255.0f};
    m.tevColors[2] = {1, 1, 1, 1};
    m.konstColors[0] = {5 / 255.0f, 5 / 255.0f, 0, 1};
    m.tevStageCount = 2;
    const std::uint8_t color[2][4]{{14, 2, 10, 15}, {15, 0, 8, 1}};
    const std::uint8_t alpha[2][4]{{4, 6, 5, 1}, {7, 7, 7, 7}};
    for (int i = 0; i < 2; ++i) {
        auto& s = m.stages[i];
        std::copy(color[i], color[i] + 4, s.colorIn);
        std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.channel = 4;
        s.texMap = s.texCoord = 0;
        s.konstAlpha = 0x05;
    }
    m.stages[0].colorScale = 2;
    m.stages[0].alphaScale = 1;
    m.stages[1].colorScale = 1;
    const auto program = smg::compileTev(m);
    if (!program.exact) dump(m, program, randomInputs(rng));
    check(program.exact, "grass with a negative register translates exactly");
    for (int i = 0; i < 200; ++i) {
        const auto in = randomInputs(rng);
        if (!same(gx(m, in), pica(program, in))) dump(m, program, in);
        check(same(gx(m, in), pica(program, in)), "grass matches GX");
    }
    // Lit, opaque grass: the alpha term is 2 (3/8 - 1/2) < 0, so nothing is added and the texture shows.
    Inputs lit{};
    lit.texture[0] = {0.3f, 0.3f, 0.2f, 1};
    lit.raster = {1, 1, 1, 1};
    const auto out = pica(program, lit);
    check(std::fabs(out[1] - 2 * 4 * (56 / 255.0f) * 0.3f) < 1e-3f && out[0] < 0.9f, "lit grass keeps its texture");
}

// Mario's EyeLid_v: the last alpha is 1 + texA/4 + 1/2, unclamped, which wraps to just under the alpha
// test's 128 where the texture is clear, so the eye shows through the lid's hole.
void marioEyeLid(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0;
    m.tevColors[0] = {0, 0, 0, -102 / 255.0f};
    m.tevColors[1] = {0, 0, 0, 51 / 255.0f};
    m.tevColors[2] = {1, 1, 1, 1};
    m.konstColors[0] = {1, 1, 1, 10 / 255.0f};
    m.tevStageCount = 3;
    const std::uint8_t color[3][4]{{15, 2, 10, 2}, {15, 10, 1, 0}, {4, 8, 10, 0}};
    const std::uint8_t alpha[3][4]{{4, 2, 5, 1}, {7, 6, 6, 7}, {7, 4, 6, 0}};
    const std::uint8_t konst[3][2]{{0x0c, 0x05}, {0x07, 0x00}, {0x0c, 0x06}};
    for (int i = 0; i < 3; ++i) {
        auto& s = m.stages[i];
        std::copy(color[i], color[i] + 4, s.colorIn);
        std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.channel = 4;
        s.texMap = s.texCoord = 0;
        s.konstColor = konst[i][0], s.konstAlpha = konst[i][1];
    }
    m.stages[0].alphaScale = 2;
    m.stages[2].colorScale = 1;
    m.stages[2].alphaBias = 1;
    m.stages[2].alphaClamp = 0;
    const auto program = smg::compileTev(m);
    if (!program.exact) dump(m, program, randomInputs(rng));
    check(program.exact, "the eyelid's wrapping alpha translates exactly");
    for (int i = 0; i < 200; ++i) {
        const auto in = randomInputs(rng);
        if (!same(gx(m, in), pica(program, in))) dump(m, program, in);
        check(same(gx(m, in), pica(program, in)), "the eyelid matches GX");
    }
    Inputs hole{};
    hole.raster = {1, 1, 1, 1};
    // GX's bias is 128 steps where this model adds half of 255, so the hardware's 127 shows here as 126.5.
    check(pica(program, hole)[3] * 255 < 127.5f, "a clear texel wraps below the alpha test's 128");
    hole.texture[0] = {1, 1, 1, 1};
    check(pica(program, hole)[3] * 255 >= 128, "an opaque texel passes it");
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

// FallOutFieldDraw's fill: alpha is TEXA > K0.a ? A0 : 0, and with A0 zero the alpha test after it passes nothing.
void compareWithoutC(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0;
    m.tevStageCount = 1;
    m.tevColors[0] = {0.5f, 0, 0, 0};
    m.konstColors[0] = {0, 0, 0, 1 / 255.0f};
    auto& s = m.stages[0];
    const std::uint8_t color[4]{2, 15, 15, 15}, alpha[4]{4, 6, 1, 7};
    std::copy(color, color + 4, s.colorIn);
    std::copy(alpha, alpha + 4, s.alphaIn);
    s.alphaOp = 14;  // GX_TEV_COMP_A8_GT
    s.konstAlpha = 0x1c;
    s.texMap = s.texCoord = 0;
    s.channel = 0xff;
    const auto program = smg::compileTev(m);
    check(program.exact, "a compare that adds nothing translates exactly");
    for (int i = 0; i < 200; ++i) {
        const auto in = randomInputs(rng);
        const Rgba out = pica(program, in);
        check(same(gx(m, in), out) && out[3] == 0, "and leaves alpha zero, as GX does");
    }
}

// Inputs as the PICA sees them, in whole steps of 1/255, with ties between texture, raster and konst alpha.
Inputs byteInputs(std::mt19937& rng) {
    std::uniform_int_distribution<int> byte(0, 255), tie(0, 3);
    Inputs in;
    for (auto& t : in.texture) for (auto& c : t) c = byte(rng) / 255.0f;
    for (auto& c : in.raster) c = byte(rng) / 255.0f;
    if (!tie(rng)) in.texture[0][3] = in.raster[3];
    if (!tie(rng)) in.texture[0][3] = 128 / 255.0f;
    if (!tie(rng)) in.texture[0][0] = in.raster[0];
    return in;
}

// The title's dissolve: alpha is TEXA > K.a ? RASA : 0, then by color: RGB8 adds K where TEXC > RASC to the texture's
// own color, R8 adds C0 where the texture's red passes the raster's, and the bloom's bright pass is that test alone.
void compareAddsC(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0;
    m.konstColors[0] = {0.25f, 0.5f, 0.75f, 128 / 255.0f};
    m.tevColors[0] = {0.2f, 0.4f, 0.6f, 1};
    const std::uint8_t colors[4][4]{{10, 15, 15, 15}, {8, 10, 14, 8}, {8, 10, 2, 15}, {8, 10, 12, 15}}, ops[4]{0, 14, 8, 8};
    for (int t = 0; t < 4; ++t) {
        m.tevStageCount = 1;
        auto& s = m.stages[0];
        s = {};
        std::copy(colors[t], colors[t] + 4, s.colorIn);
        const std::uint8_t alpha[4]{4, 6, 5, 7};
        std::copy(alpha, alpha + 4, s.alphaIn);
        s.colorOp = ops[t];
        s.alphaOp = 14;  // GX_TEV_COMP_A8_GT
        s.konstColor = 0x0c;
        s.konstAlpha = 0x1c;
        s.texMap = s.texCoord = 0;
        s.channel = 4;
        const auto program = smg::compileTev(m);
        check(program.exact && program.stageCount <= 6, "a compare that adds c translates exactly");
        for (int i = 0; i < 300; ++i) {
            const auto in = byteInputs(rng);
            if (same(gx(m, in), pica(program, in))) continue;
            dump(m, program, in);
            check(false, "and matches GX on byte inputs, ties included");
        }
    }
}

// BloomEffect's blur: six stages each add one tap of the same map (its own coordinate) weighted by the rasterized color.
// The PICA has three units; the extra taps share one, so black stays black instead of each adding white.
void sixTapBlur(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0;
    m.tevStageCount = 6;
    for (int i = 0; i < 6; ++i) {
        auto& s = m.stages[i];
        const std::uint8_t color[4]{15, 8, 10, std::uint8_t(i ? 0 : 15)}, alpha[4]{7, 7, 7, 6};
        std::copy(color, color + 4, s.colorIn);
        std::copy(alpha, alpha + 4, s.alphaIn);
        s.colorClamp = i == 5;
        s.texMap = 0, s.texCoord = static_cast<std::uint8_t>(i), s.channel = 4;
    }
    m.konstColors[0] = {1, 1, 1, 1};
    const auto program = smg::compileTev(m);
    check(program.unitCount == 3 && program.units[2].texture == 0, "six taps of one map share three units");
    Inputs black = randomInputs(rng);
    black.texture[0] = {0, 0, 0, 1};
    black.raster = {0.16f, 0.16f, 0.16f, 1};
    const Rgba dark = pica(program, black);
    check(dark[0] == 0 && dark[1] == 0 && dark[2] == 0, "a black source blurs to black");
    for (int i = 0; i < 100; ++i) {
        Inputs in = randomInputs(rng);
        for (auto& c : in.raster) c *= 0.16f;
        const Rgba g = gx(m, in), q = pica(program, in);
        bool within = true;  // six stages hold five taps here, so a sixth of the weight may go, but nothing is added
        for (int c = 0; c < 3; ++c) within &= q[c] <= g[c] + 1 / 255.0f && q[c] >= g[c] * 5 / 6 - 1 / 255.0f;
        check(within, "and the taps that fit keep their weight, never adding light");
    }
}

void randomMaterials(std::mt19937& rng) {
    std::uniform_int_distribution<int> colorInput(0, 15), alphaInput(0, 7), small(0, 3), stageCount(1, 4), selection(0, 0x1f);
    std::uniform_real_distribution<float> unit(0, 1), wide(-1.5f, 2.0f);
    unsigned exact = 0, exactSigned = 0;
    constexpr unsigned trials = 8000;
    for (unsigned t = 0; t < trials; ++t) {
        smg::ModelMaterial m;
        // Half the trials use registers as the disc does: signed, past one, and stages that skip the clamp.
        const bool signedRegisters = t % 2;
        for (int i = 0; i < 4; ++i) {
            m.textures[i] = i;
            for (int c = 0; c < 4; ++c) {
                // Registers hold whole signed steps of 1/255, as J3D stores them.
                m.tevColors[i][c] = signedRegisters ? std::round(wide(rng) * 255) / 255 : unit(rng);
                m.konstColors[i][c] = unit(rng);
            }
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
            s.colorClamp = !signedRegisters || small(rng);
            s.alphaClamp = !signedRegisters || small(rng);
        }
        const auto program = smg::compileTev(m);
        check(program.stageCount >= 1 && program.stageCount <= 6 && program.unitCount <= 3, "program fits the hardware");
        if (!program.exact) continue;
        ++exact;
        exactSigned += signedRegisters;
        for (int i = 0; i < 20; ++i) {
            const auto in = randomInputs(rng);
            if (same(gx(m, in), pica(program, in))) continue;
            std::fprintf(stderr, "material %u\n", t);
            dump(m, program, in);
            check(false, "exact program matches GX");
        }
    }
    std::printf("%u of %u random materials translated exactly, %u with signed registers\n", exact, trials, exactSigned);
    check(exact > trials / 50, "random materials exercise the exact path");
    check(exactSigned > trials / 100, "signed registers exercise the exact path");
}
} // namespace

// OceanBowl::loadMaterial (Beach Bowl's water), as the decomp sets it: two samples of the water multiplied and halved,
// C0 added where that product's red equals A0 (R8_EQ: the bright lines), the screen behind (warped) times C1 added,
// alpha the vertex's times a mask's. Four samples and the compare are more than one PICA pass holds.
smg::ModelMaterial oceanBowl() {
    smg::ModelMaterial m;
    m.textures[0] = 0, m.textures[1] = 1, m.textures[3] = 3, m.textures[4] = 4;  // map 4: the second sample, apart
    m.tevColors[0] = {40 / 255.0f, 40 / 255.0f, 40 / 255.0f, 20 / 255.0f};
    m.tevColors[1] = {200 / 255.0f, 230 / 255.0f, 210 / 255.0f, 1};
    m.tevStageCount = 4;
    const std::uint8_t color[4][4]{{8, 15, 15, 15}, {15, 8, 0, 15}, {0, 3, 2, 0}, {15, 8, 4, 0}};
    const std::uint8_t alpha[4][4]{{4, 7, 7, 7}, {7, 4, 0, 7}, {7, 5, 4, 7}, {0, 7, 7, 7}};
    const std::uint8_t op[4]{0, 0, 9, 0}, colorScale[4]{0, 3, 0, 0}, alphaScale[4]{0, 1, 0, 0}, clamp[4]{0, 0, 0, 1};
    const std::uint8_t map[4]{0, 4, 3, 1}, coord[4]{0, 1, 4, 3}, channel[4]{0xff, 0xff, 4, 0xff};
    for (int i = 0; i < 4; ++i) {
        auto& t = m.stages[i];
        t = {};
        std::copy(color[i], color[i] + 4, t.colorIn), std::copy(alpha[i], alpha[i] + 4, t.alphaIn);
        t.colorOp = op[i], t.colorScale = colorScale[i], t.alphaScale = alphaScale[i], t.colorClamp = t.alphaClamp = clamp[i];
        t.texMap = map[i], t.texCoord = coord[i], t.channel = channel[i];
    }
    m.blend = {1, 4, 5, 0};  // source alpha over the rest
    m.alphaTest = {4, 0, 1, 4, 0};  // alpha above zero
    return m;
}

void waterPasses(std::mt19937& rng) {
    const auto m = oceanBowl();
    smg::TevPasses passes;
    check(!smg::compileTev(m).exact && smg::splitTev(m, passes) && passes.extra.size() == 2,
          "water no pass holds splits into a first pass, its bright lines and its screen term");
    std::vector<smg::CombinerProgram> programs{smg::compileTev(passes.first)};
    for (const auto& pass : passes.extra) programs.push_back(smg::compileTev(pass.material));
    bool exact = true;
    for (const auto& p : programs) exact &= p.exact && p.stageCount <= smg::CombinerProgram::MaxStages;
    check(exact, "and each pass translates exactly");
    std::uniform_int_distribution<int> byte(0, 255), near(38, 42);
    unsigned compared = 0, lines = 0;
    for (int i = 0; i < 2000; ++i) {
        auto in = byteInputs(rng);
        if (i & 1) in.texture[4] = {1, 1, 1, byte(rng) / 255.0f}, in.texture[0][0] = near(rng) / 255.0f;  // near the lines
        const Rgba dst{byte(rng) / 255.0f, byte(rng) / 255.0f, byte(rng) / 255.0f, 1};
        const Rgba g = gx(m, in);
        if (std::max({g[0], g[1], g[2]}) >= 1 - 1e-6f) continue;  // GX's sum saturated once; the passes' would apart
        Rgba want = dst, got = dst;
        if (g[3] > 0)
            for (int c = 0; c < 3; ++c) want[c] = g[c] * g[3] + dst[c] * (1 - g[3]);
        const Rgba first = pica(programs[0], in);
        if (first[3] > 0)
            for (int c = 0; c < 3; ++c) got[c] = first[c] * first[3] + dst[c] * (1 - first[3]);
        for (std::size_t e = 0; e < passes.extra.size(); ++e) {
            const auto& pass = passes.extra[e];
            const Rgba x = pica(programs[e + 1], in);
            const bool drawn = pass.alphaEqual ? std::lround(x[3] * 255) == pass.reference : x[3] > 0;
            lines += drawn && pass.alphaEqual;
            if (drawn)
                for (int c = 0; c < 3; ++c) got[c] = std::min(1.0f, got[c] + (pass.alphaEqual ? x[c] : x[c] * x[3]));
        }
        ++compared;
        if (std::abs(got[0] - want[0]) <= 2 / 255.0f && std::abs(got[1] - want[1]) <= 2 / 255.0f && std::abs(got[2] - want[2]) <= 2 / 255.0f) continue;
        dump(m, programs[0], in);
        std::printf("want %.3f %.3f %.3f got %.3f %.3f %.3f\n", want[0], want[1], want[2], got[0], got[1], got[2]);
        check(false, "and the passes blended match GX's water blended");
        break;
    }
    check(compared > 1000 && lines > 50, "over inputs on and off the bright lines");
}

// ElectricRailMoving's SparkMat, from the disc: (C0 + TEXC x C2 - 1/2) x 4, then A1 > the second map's red ? that : 0.
// Its first op multiplies a texture by a constant; the zero it leaves unused once took the other constant's slot.
void electricRail(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0, m.textures[1] = 1;
    m.tevColors[0] = {0.471f, 0.251f, 0, 1};
    m.tevColors[1] = {1, 1, 1, 1};
    m.tevColors[2] = {0.392f, 0.392f, 0.314f, 1};
    m.tevStageCount = 2;
    const std::uint8_t color[2][4]{{15, 8, 6, 2}, {5, 8, 0, 15}}, alpha[2][4]{{4, 7, 7, 7}, {7, 7, 7, 7}};
    for (int i = 0; i < 2; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = s.texCoord = std::uint8_t(i);
    }
    m.stages[0].colorBias = 2, m.stages[0].colorScale = 2;
    m.stages[1].colorOp = 8, m.stages[1].colorBias = 3;  // R8_GT, as J3D keeps a compare
    const auto program = smg::compileTev(m);
    check(program.exact && program.stageCount <= smg::CombinerProgram::MaxStages, "the electric rail's spark translates exactly");
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "and matches GX on byte inputs");
        break;
    }
}

// Buoy Base's Gearmo body (caretaker.bdl BodyMat_v, C0 as bodycolor.brk's yellow): lit texture plus the tint, its
// alpha's highlight added twice, the environment map, the soot taken off. The last alpha saturates whatever it reads,
// so the alpha stages before it take no combiner stages; cut short at six, the body came out black.
void gearmoBody(std::mt19937& rng) {
    smg::ModelMaterial m;
    for (int i = 0; i < 3; ++i) m.textures[i] = i;
    m.tevColors[0] = {45 / 255.0f, 40 / 255.0f, -31 / 255.0f, -60 / 255.0f};
    m.tevColors[1] = {0, 0, 0, -150 / 255.0f};
    m.tevColors[2] = {1023 / 255.0f, 1023 / 255.0f, 1023 / 255.0f, 1};
    m.konstColors[1] = m.konstColors[2] = m.konstColors[3] = {1, 1, 1, 1};
    m.tevStageCount = 4;
    const std::uint8_t color[4][4]{{4, 8, 10, 2}, {15, 10, 1, 0}, {15, 8, 10, 0}, {15, 6, 8, 0}};
    const std::uint8_t alpha[4][4]{{4, 6, 5, 1}, {7, 6, 5, 1}, {0, 7, 7, 7}, {7, 6, 2, 0}};
    const std::uint8_t map[4]{0, 0xff, 1, 2}, konst[4][2]{{0x0c, 0x07}, {0x07, 0x06}, {0x00, 0x1c}, {0x00, 0x00}};
    for (int i = 0; i < 4; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = s.texCoord = map[i], s.channel = 4, s.konstColor = konst[i][0], s.konstAlpha = konst[i][1];
    }
    m.stages[0].alphaScale = m.stages[1].colorScale = m.stages[1].alphaScale = 1;
    m.stages[2].alphaClamp = 0;
    m.stages[3].colorOp = 1, m.stages[3].alphaScale = 2;
    const auto program = smg::compileTev(m);
    check(program.exact && program.stageCount <= smg::CombinerProgram::MaxStages, "the Gearmo's body translates exactly");
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "and matches GX on byte inputs");
        break;
    }
}

// Good Egg's rolling Chain Chomp (wanwanrolling.bdl WanwanRollingBody): an alpha chain of four ops beside a color chain of
// two in the first two stages. Kept in step, the channels took eight combiner stages; each following its own ops, six.
void chainChompBody(std::mt19937& rng) {
    smg::ModelMaterial m;
    for (int i = 0; i < 3; ++i) m.textures[i] = i;
    m.tevColors[0] = {1, 1, 1, -134 / 255.0f};
    m.tevColors[1] = {0, 0, 0, 106 / 255.0f};
    m.tevColors[2] = {0, 0, 0, 1};
    m.konstColors[0] = {1, 1, 1, 10 / 255.0f};
    m.konstColors[1] = m.konstColors[2] = m.konstColors[3] = {1, 1, 1, 1};
    m.tevStageCount = 3;
    const std::uint8_t color[3][4]{{15, 8, 10, 15}, {15, 2, 3, 0}, {1, 10, 8, 0}};
    const std::uint8_t alpha[3][4]{{4, 2, 5, 1}, {7, 5, 0, 7}, {0, 7, 7, 7}};
    const std::uint8_t map[3]{0, 0, 2}, konst[3][2]{{0x0c, 0x05}, {0x07, 0x00}, {0x0c, 0x1c}};
    for (int i = 0; i < 3; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = s.texCoord = map[i], s.channel = 4, s.konstColor = konst[i][0], s.konstAlpha = konst[i][1];
    }
    m.stages[0].alphaScale = 1;
    m.stages[1].colorBias = 2, m.stages[1].colorScale = 3, m.stages[1].alphaScale = 3, m.stages[1].alphaClamp = 0;
    m.stages[2].colorScale = 1, m.stages[2].alphaClamp = 0;
    const auto program = smg::compileTev(m);
    check(program.exact && program.stageCount <= smg::CombinerProgram::MaxStages, "the Chain Chomp's body translates exactly");
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "and matches GX on byte inputs");
        break;
    }
}

// Kingfin's bones: stage 1's unclamped sum is read two combiner stages after it's made, once the buffer already kept
// stage 0's output for stage 1. The buffer is written a second time, as the first value's readers are done by then.
void kingfinBone(std::mt19937& rng) {
    smg::ModelMaterial m;
    for (int i = 0; i < 2; ++i) m.textures[i] = i;
    m.tevColors[0] = {133 / 255.0f, 133 / 255.0f, 155 / 255.0f, 0};
    m.tevColors[1] = {43 / 255.0f, 45 / 255.0f, 32 / 255.0f, 0};
    m.tevColors[2] = {30 / 255.0f, 30 / 255.0f, 30 / 255.0f, 1};
    m.konstColors[0] = {77 / 255.0f, 64 / 255.0f, 51 / 255.0f, 51 / 255.0f};
    m.tevStageCount = 3;
    const std::uint8_t color[3][4]{{14, 2, 10, 15}, {6, 0, 8, 1}, {10, 4, 8, 0}};
    const std::uint8_t alpha[3][4]{{6, 7, 5, 7}, {7, 7, 7, 7}, {5, 7, 7, 7}};
    const std::uint8_t map[3]{0xFF, 0, 1}, konst[3][2]{{0x0c, 0x1c}, {0x0e, 0x05}, {0x0e, 0x1c}};
    for (int i = 0; i < 3; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = map[i], s.texCoord = map[i] == 0xFF ? 0 : map[i], s.channel = 4, s.konstColor = konst[i][0], s.konstAlpha = konst[i][1];
    }
    m.stages[0].alphaScale = 2;
    m.stages[1].colorClamp = 0;
    m.stages[2].colorBias = 2, m.stages[2].colorScale = 1, m.stages[2].alphaClamp = 0;
    const auto program = smg::compileTev(m);
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "Kingfin's bones match GX on byte inputs");
        break;
    }
}

// The Starshroom's hull: vertex color plus a lerp minus a half, unclamped (up to 1.5), then plus vertex color times
// a texture minus a half, saturated. Kept at half scale, the sum saturates only where GX's does.
void starshroomHull(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0, m.textures[2] = 2;
    m.tevColors[0] = {220 / 255.0f, 50 / 255.0f, 30 / 255.0f, 1};
    m.tevColors[1] = {121 / 255.0f, 32 / 255.0f, 36 / 255.0f, 1};
    m.tevColors[2] = {1, 1, 1, 1};
    m.tevStageCount = 2;
    const std::uint8_t color[2][4]{{2, 4, 8, 10}, {15, 10, 8, 0}};
    const std::uint8_t alpha[2][4]{{7, 4, 5, 7}, {5, 7, 7, 7}};
    for (int i = 0; i < 2; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = s.texCoord = static_cast<std::uint8_t>(i * 2), s.channel = 4, s.colorBias = 2;
    }
    m.stages[0].colorClamp = 0;
    m.stages[1].alphaClamp = 0;
    const auto program = smg::compileTev(m);
    check(program.exact, "the Starshroom's hull translates exactly");
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "and matches GX on byte inputs");
        break;
    }
}

// The valve switch: stage 1 writes its color to C2, so stage 2 reads C2 as the combiner's previous output and the
// older PREV from the buffer, kept there by the stage that made it.
void valveSwitch(std::mt19937& rng) {
    smg::ModelMaterial m;
    for (int i = 0; i < 3; ++i) m.textures[i] = i;
    m.tevStageCount = 3;
    const std::uint8_t color[3][4]{{15, 10, 8, 15}, {15, 8, 14, 15}, {15, 8, 6, 0}};
    const std::uint8_t alpha[3][4]{{7, 5, 4, 7}, {5, 7, 7, 7}, {5, 7, 7, 7}};
    const std::uint8_t map[3]{0, 2, 1};
    for (int i = 0; i < 3; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = s.texCoord = map[i], s.channel = 4;
    }
    m.stages[1].konstColor = 0x06, m.stages[1].colorReg = 3, m.stages[1].alphaClamp = 0;
    m.stages[2].colorScale = 1, m.stages[2].alphaClamp = 0;
    const auto program = smg::compileTev(m);
    check(program.exact, "the valve switch translates exactly");
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "and matches GX on byte inputs");
        break;
    }
}

// HalfGalaxySky's blue: C0 minus texture times vertex color, with C0 past one (1.04, 1.47, 1.89 by component).
void skyPastOne(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0, m.textures[1] = 1;
    m.tevColors[0] = {265 / 255.0f, 375 / 255.0f, 481 / 255.0f, 1};
    m.tevStageCount = 2;
    const std::uint8_t color[2][4]{{15, 8, 10, 2}, {15, 14, 8, 0}};
    const std::uint8_t alpha[2][4]{{7, 4, 5, 7}, {5, 7, 7, 7}};
    for (int i = 0; i < 2; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = s.texCoord = static_cast<std::uint8_t>(i), s.channel = 4;
    }
    m.stages[0].colorOp = 1;
    m.stages[1].alphaClamp = 0;
    const auto program = smg::compileTev(m);
    check(program.exact, "a register past one minus a term translates exactly");
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "and matches GX on byte inputs");
        break;
    }
}

// A flag disc planet's red panel: stage 1 lerps C1 and C0 by C0's negative alpha (wrapped as a c input), three
// constants that are one, which leaves room for the others.
void flagPanel(std::mt19937& rng) {
    smg::ModelMaterial m;
    m.textures[0] = 0, m.textures[1] = 1;
    m.tevColors[0] = {255 / 255.0f, 202 / 255.0f, 218 / 255.0f, -89 / 255.0f};
    m.tevColors[1] = {113 / 255.0f, 165 / 255.0f, 72 / 255.0f, 88 / 255.0f};
    m.tevColors[2] = {0, 0, 0, 1};
    m.tevStageCount = 3;
    const std::uint8_t color[3][4]{{15, 8, 10, 15}, {4, 2, 3, 0}, {1, 10, 8, 0}};
    const std::uint8_t alpha[3][4]{{4, 2, 5, 1}, {7, 5, 0, 7}, {0, 7, 7, 7}};
    const std::uint8_t map[3]{0, 0, 1}, konst[3][2]{{0x0c, 0x05}, {0x07, 0x00}, {0x0c, 0x1c}};
    for (int i = 0; i < 3; ++i) {
        auto& s = m.stages[i];
        s = {};
        std::copy(color[i], color[i] + 4, s.colorIn), std::copy(alpha[i], alpha[i] + 4, s.alphaIn);
        s.texMap = s.texCoord = map[i], s.channel = 4, s.konstColor = konst[i][0], s.konstAlpha = konst[i][1];
    }
    m.stages[0].alphaScale = 1;
    m.stages[1].colorBias = 2, m.stages[1].colorScale = 3, m.stages[1].alphaScale = 3, m.stages[1].alphaClamp = 0;
    m.stages[2].colorScale = 1, m.stages[2].alphaClamp = 0;
    const auto program = smg::compileTev(m);
    for (int i = 0; i < 300; ++i) {
        const auto in = byteInputs(rng);
        if (same(gx(m, in), pica(program, in))) continue;
        dump(m, program, in);
        check(false, "a flag panel's lerp of constants matches GX on byte inputs");
        break;
    }
}

int runCombinerTests() {
    std::mt19937 rng(1234);
    realMaterial(rng);
    diskGardenGrass(rng);
    marioEyeLid(rng);
    previousTwice(rng);
    kingfinBone(rng);
    starshroomHull(rng);
    valveSwitch(rng);
    skyPastOne(rng);
    flagPanel(rng);
    compareWithoutC(rng);
    compareAddsC(rng);
    sixTapBlur(rng);
    waterPasses(rng);
    electricRail(rng);
    gearmoBody(rng);
    chainChompBody(rng);
    randomMaterials(rng);
    std::printf("%u TEV combiner checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runCombinerTests(); }
#endif
