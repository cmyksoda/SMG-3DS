#include <smg/GxFog.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace smg {
namespace {
// A, and C with its projection and type: an 11-bit mantissa, 8-bit exponent and sign, a float's top bits.
float fogFloat(std::uint32_t word) {
    const std::uint32_t bits = (word >> 19 & 1) << 31 | (word >> 11 & 0xFF) << 23 | (word & 0x7FF) << 12;
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
} // namespace

GxFog readGxFog(const std::uint32_t words[6]) {
    GxFog fog;
    const std::uint32_t a = words[1], c = words[4], color = words[5];
    fog.type = c >> 21 & 7;
    fog.orthographic = c >> 20 & 1;
    fog.rangeAdjust = words[0] >> 10 & 1;
    fog.bMagnitude = words[2] & 0xFFFFFF;
    fog.bShift = words[3] & 0x1F;
    // Both exponents all ones: Dolphin's reading of hardware, no A and an infinite C.
    if ((a >> 11 & 0xFF) == 0xFF && (c >> 11 & 0xFF) == 0xFF) {
        const float inf = std::numeric_limits<float>::infinity();
        fog.a = 0, fog.c = !(a >> 19 & 1) && !(c >> 19 & 1) ? -inf : inf;
    } else {
        fog.a = fogFloat(a), fog.c = fogFloat(c);
    }
    fog.color[0] = color >> 16 & 0xFF, fog.color[1] = color >> 8 & 0xFF, fog.color[2] = color & 0xFF;
    return fog;
}

float gxFogShare(const GxFog& fog, std::uint32_t z) {
    if (!fog.type) return 0;
    float ze;
    if (fog.orthographic) {
        ze = fog.a * (static_cast<float>(z) / 16777215.0f);
    } else {
        const std::int32_t denominator = static_cast<std::int32_t>(fog.bMagnitude) - static_cast<std::int32_t>(z >> fog.bShift);
        ze = fog.a * 16777215.0f / static_cast<float>(denominator);
    }
    float amount = std::clamp(ze - fog.c, 0.0f, 1.0f);
    switch (fog.type) {
    case 4: amount = 1.0f - std::pow(2.0f, -8.0f * amount); break;
    case 5: amount = 1.0f - std::pow(2.0f, -8.0f * amount * amount); break;
    case 6: amount = std::pow(2.0f, -8.0f * (1.0f - amount)); break;
    case 7: amount = std::pow(2.0f, -8.0f * (1.0f - amount) * (1.0f - amount)); break;
    default: break;
    }
    return amount;
}

unsigned gxFogAmount(const GxFog& fog, std::uint32_t z) { return static_cast<unsigned>(gxFogShare(fog, z) * 256); }

namespace {
float keptAt(const GxFog& fog, double depth) {
    return 1 - gxFogShare(fog, static_cast<std::uint32_t>(std::lround(depth * 16777215.0)));
}
} // namespace

void picaFogTable(const GxFog& fog, float table[256]) {
    // A step is a straight line, and far fog rises steeply inside the last ones (most of a scene lies past depth 0.99):
    // each line runs from GX's value to the next step's unless that fogs more than GX by over 6/256 somewhere, then as
    // steeply as stays within it. Too little far fog shows far less than a far scene washed white.
    constexpr unsigned Samples = 32;
    constexpr float Over = 6.0f / 256;
    for (unsigned i = 0; i < 128; ++i) {
        const float start = keptAt(fog, i / 128.0);
        float change = keptAt(fog, (i + 1) / 128.0) - start;
        for (unsigned k = 1; k < Samples; ++k) {
            const float fraction = static_cast<float>(k) / Samples;
            change = std::max(change, (keptAt(fog, (i + fraction) / 128.0) - Over - start) / fraction);
        }
        table[i] = start, table[128 + i] = change;
    }
}
} // namespace smg
