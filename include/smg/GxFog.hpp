#pragma once
// GX fog (BP 0xEE to 0xF2) as the PICA's fog unit takes it: a table of 128 steps over depth.
#include <cstdint>

namespace smg {
struct GxFog {
    unsigned type = 0;  // TEV_FOG_PARAM_3's FSEL: 0 off, 2 linear, 4 and 5 exponential, 6 and 7 their reverses
    bool orthographic = false, rangeAdjust = false;
    float a = 0, c = 0;
    std::uint32_t bMagnitude = 0, bShift = 0;
    std::uint8_t color[3]{};  // red, green, blue
};

// BP 0xE8 (range adjustment), then 0xEE to 0xF2 (A, B's magnitude and shift, C with the type, the color).
constexpr std::uint32_t GxFogRegisters[6] = {0xE8, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2};
GxFog readGxFog(const std::uint32_t words[6]);
// How much of the fog color a fragment at 24-bit screen depth `z` takes, 0 to 1, and in 256ths as the Wii's GPU blends it.
float gxFogShare(const GxFog& fog, std::uint32_t z);
unsigned gxFogAmount(const GxFog& fog, std::uint32_t z);
// The share of its own color each step keeps, then its change across the step, for FogLut_FromArray with its z flipped:
// step i starts at screen depth i/128 (the port's depth is 1 - z, so flipped it is z itself). At most 6/256 more fog than GX's.
void picaFogTable(const GxFog& fog, float table[256]);
} // namespace smg
