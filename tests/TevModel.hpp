// The TEV and the PICA200 combiner as reference models for checking translated programs: the tests and the audit
// measure a program against GX with them.
#pragma once
#include <smg/Combiner.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace tevmodel {
using Rgba = std::array<float, 4>;

struct Inputs {
    Rgba texture[8]; // by model texture index
    Rgba raster;
};

inline float konst(const smg::ModelMaterial& m, std::uint8_t select, int component) {
    if (select < 8) return (8 - select) / 8.0f;
    if (select >= 0x0c && select < 0x10) return m.konstColors[select - 0x0c][component];
    if (select >= 0x10 && select < 0x20) return m.konstColors[select & 3][(select - 0x10) >> 2];
    return 1;
}

// Inputs a, b and c take a register's low eight bits, and so does the output: modulo 256 steps, kept
// continuous so it compares with the combiner's unrounded arithmetic.
inline float wrapByte(float v) {
    if (v >= 0 && v <= 1) return v;
    return v - 256 / 255.0f * std::floor(v * 255 / 256 + 1e-6f);
}

// The GX TEV as Dolphin models it: every stage computes d +/- lerp(a, b, c), then bias and scale; it
// saturates only when its clamp bit is set (else it keeps -1024..1023), and the output is taken modulo 256.
inline Rgba tev(const smg::ModelMaterial& m, const Inputs& in) {
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
inline Rgba pica(const smg::CombinerProgram& p, const Inputs& in) {
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

// GX's alpha test: two compares of the alpha byte, joined by and, or, xor or xnor.
inline bool alphaPasses(const smg::ModelMaterial& m, float alpha) {
    const long a = std::lround(alpha * 255);
    const auto compare = [a](std::uint8_t c, long r) {
        switch (c) {
        case 0: return false;
        case 1: return a < r;
        case 2: return a == r;
        case 3: return a <= r;
        case 4: return a > r;
        case 5: return a != r;
        case 6: return a >= r;
        default: return true;
        }
    };
    const auto& t = m.alphaTest;
    const bool x = compare(t.compare0, t.reference0), y = compare(t.compare1, t.reference1);
    return t.op == 0 ? x && y : t.op == 1 ? x || y : t.op == 2 ? x != y : x == y;
}

// A color c drawn over dst by the material's blend: replacing it, or by c's alpha with dst kept, added or weighed.
inline Rgba blended(const smg::ModelMaterial& m, const Rgba& c, Rgba dst) {
    if (m.blend.type != 1 || m.blend.source != 4) return c;
    const float keep = m.blend.destination == 1 ? 1 : m.blend.destination == 5 ? 1 - c[3] : 0;
    for (int i = 0; i < 3; ++i) dst[i] = std::min(1.0f, c[i] * c[3] + dst[i] * keep);
    return dst;
}

// What GX draws over dst, and what passes split from it (splitTev) draw over dst in turn as the renderer blends them.
inline Rgba drawn(const smg::ModelMaterial& m, const Inputs& in, const Rgba& dst) {
    const Rgba c = tev(m, in);
    return alphaPasses(m, c[3]) ? blended(m, c, dst) : dst;
}
inline Rgba drawnInPasses(const smg::ModelMaterial& m, const smg::TevPasses& passes, const std::vector<smg::CombinerProgram>& programs,
                          const Inputs& in, Rgba dst) {
    const Rgba first = pica(programs[0], in);
    if (alphaPasses(m, first[3])) dst = blended(m, first, dst);
    for (std::size_t e = 0; e < passes.extra.size(); ++e) {
        const smg::TevPass& pass = passes.extra[e];
        const Rgba x = pica(programs[e + 1], in);
        const bool shown = pass.blend == smg::TevPass::Over ||
                           (pass.blend == smg::TevPass::AddWhereEqual ? std::lround(x[3] * 255) == pass.reference : alphaPasses(m, x[3]));
        if (!shown) continue;
        const float by = pass.blend == smg::TevPass::AddTimesAlpha || pass.blend == smg::TevPass::Over ? x[3] : 1;
        const float keep = pass.blend == smg::TevPass::Over ? 1 - x[3] : 1;
        for (int c = 0; c < 3; ++c) dst[c] = std::min(1.0f, x[c] * by + dst[c] * keep);
    }
    return dst;
}

inline bool same(const Rgba& a, const Rgba& b) {
    for (int c = 0; c < 4; ++c) if (std::fabs(a[c] - b[c]) > 1e-4f) return false;
    return true;
}
} // namespace tevmodel
