#include <smg/GxFog.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

std::uint32_t bitsOf(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

// The registers GXSetFog (GXPixel.c) writes for a perspective fog.
void setFog(std::uint32_t* bp, unsigned type, float startz, float endz, float nearz, float farz, std::uint32_t rgb) {
    const float A = farz * nearz / ((farz - nearz) * (endz - startz)), B = farz / (farz - nearz), C = startz / (endz - startz);
    float mantissa = B;
    int exponent = 0;
    while (mantissa > 1.0) mantissa /= 2, ++exponent;
    while (mantissa > 0 && mantissa < 0.5) mantissa *= 2, --exponent;
    const std::uint32_t a = bitsOf(A / static_cast<float>(1 << (exponent + 1))), c = bitsOf(C);
    bp[0xEE] = (a >> 12 & 0x7FF) | (a >> 23 & 0xFF) << 11 | (a >> 31) << 19;
    bp[0xEF] = static_cast<std::uint32_t>(mantissa * 8388638);
    bp[0xF0] = static_cast<std::uint32_t>(exponent + 1);
    bp[0xF1] = (c >> 12 & 0x7FF) | (c >> 23 & 0xFF) << 11 | (c >> 31) << 19 | (type & 7) << 21 | (type >> 3 & 1) << 20;
    bp[0xF2] = rgb;
}

// The 24-bit screen depth of a point `distance` in front of the camera under GX's perspective projection.
std::uint32_t screenDepth(double distance, double nearz, double farz) {
    return static_cast<std::uint32_t>(std::lround(16777215.0 * farz * (1 - nearz / distance) / (farz - nearz)));
}

smg::GxFog read(const std::uint32_t* bp) {
    std::uint32_t words[6];
    for (int i = 0; i < 6; ++i) words[i] = bp[smg::GxFogRegisters[i]];
    return smg::readGxFog(words);
}
} // namespace

int runGxFogTests() {
    std::uint32_t bp[0x100]{};
    setFog(bp, 2, 100, 1100, 10, 100000, 0x2040FF);
    smg::GxFog fog = read(bp);
    check(fog.type == 2 && !fog.orthographic && !fog.rangeAdjust, "a linear perspective fog reads as one");
    check(fog.color[0] == 0x20 && fog.color[1] == 0x40 && fog.color[2] == 0xFF, "the fog color reads red, green, blue");
    const unsigned half = smg::gxFogAmount(fog, screenDepth(600, 10, 100000));
    check(half >= 126 && half <= 130, "halfway between start and end takes half the fog color");
    check(smg::gxFogAmount(fog, screenDepth(50, 10, 100000)) == 0, "nearer than the start takes none");
    check(smg::gxFogAmount(fog, screenDepth(5000, 10, 100000)) == 256, "beyond the end takes all of it");

    float table[256];
    smg::picaFogTable(fog, table);
    check(table[0] == 1.0f, "the near plane keeps its own color");
    for (unsigned i = 0; i < 127; ++i) check(table[i + 1] <= table[i], "a linear fog only thickens with depth");
    // At most 6/256 more fog than GX's anywhere in a step, as the PICA reads it (and GX's own 256ths).
    for (unsigned i = 0; i < 128; ++i)
        for (unsigned k = 0; k < 64; ++k) {
            const double depth = (i + k / 64.0) / 128;
            const double gx = (256 - smg::gxFogAmount(fog, static_cast<std::uint32_t>(std::lround(depth * 16777215.0)))) / 256.0;
            check(table[i] + table[128 + i] * (k / 64.0) >= gx - 8.0 / 256, "a step never fogs much more than GX");
        }
    // The PICA reads a step's value plus its change times the depth's fraction of the step.
    const double at = screenDepth(600, 10, 100000) / 16777215.0 * 128;
    const unsigned step = static_cast<unsigned>(at);
    const double kept = table[step] + table[128 + step] * (at - step);
    check(std::fabs(kept - 0.5) < 0.03, "between its steps the table keeps about half at the halfway depth");

    // Battlerock's white haze, 0 to 200000: almost none at a few thousand units, which the last step must not wash white.
    setFog(bp, 2, 0, 200000, 10, 100000, 0xFFFFFF);
    fog = read(bp);
    smg::picaFogTable(fog, table);
    const double far = screenDepth(5000, 10, 100000) / 16777215.0 * 128;
    const unsigned farStep = static_cast<unsigned>(far);
    const double farKept = table[farStep] + table[128 + farStep] * (far - farStep);
    check(farKept > 0.95, "a faint far haze stays faint in the table's last steps");

    setFog(bp, 4, 100, 1100, 10, 100000, 0);
    fog = read(bp);
    const unsigned exp = smg::gxFogAmount(fog, screenDepth(600, 10, 100000));
    check(exp >= 238 && exp <= 241, "an exponential fog halfway takes 1 - 2^-4 of the color");

    bp[0xEE] = 0xFF << 11, bp[0xF1] = 0xFF << 11 | 2 << 21;
    fog = read(bp);
    check(smg::gxFogAmount(fog, 1000) == 256, "both exponents all ones is fog everywhere, as Dolphin reads it");

    bp[0xF1] = 0;
    check(smg::gxFogAmount(read(bp), 1000) == 0, "fog off takes none");
    std::printf("%u GX fog checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGxFogTests(); }
#endif
