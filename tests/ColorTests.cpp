#include <JSystem/JUtility/TColor.hpp>
#include <nw4r/ut/Color.h>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

void packing() {
    const JUtility::TColor packed(0x11223344);
    check(packed.r == 0x11 && packed.g == 0x22 && packed.b == 0x33 && packed.a == 0x44, "a TColor from 0xRRGGBBAA has R on top, as on the Wii");
    check(static_cast<u32>(packed) == 0x11223344, "a TColor reads back as 0xRRGGBBAA");
    const JUtility::TColor white;
    check(white.r == 0xFF && white.g == 0xFF && white.b == 0xFF && white.a == 0xFF, "a default TColor is opaque white");
    const nw4r::ut::Color colour(0x000000FFu);
    check(colour.r == 0 && colour.g == 0 && colour.b == 0 && colour.a == 0xFF, "an nw4r colour from 0x000000FF is opaque black");
    check(static_cast<u32>(colour) == 0x000000FFu, "an nw4r colour reads back as 0xRRGGBBAA");
    const GXColor bytes = {1, 2, 3, 4};
    const nw4r::ut::Color fromBytes(bytes);
    check(fromBytes.r == 1 && fromBytes.a == 4 && static_cast<u32>(fromBytes) == 0x01020304, "an nw4r colour keeps a GXColor's bytes");
}
} // namespace

int runColorTests() {
    packing();
    std::printf("%u colour packing checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runColorTests(); }
#endif
