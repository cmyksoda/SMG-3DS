#include <JSystem/J3DGraphBase/J3DShapeDraw.hpp>
#include <JSystem/J3DGraphBase/J3DTevs.hpp>
#include <JSystem/JUtility/TColor.hpp>
#include <nw4r/ut/Color.h>
#include <revolution/gd/GDBase.h>
#include <smg/GxCommands.hpp>
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

// A TEV stage's two BP commands, through GD into the port's command processor.
void tevStage() {
    static std::uint8_t list[64];
    GDLObj object;
    GDInitGDLObj(&object, list, sizeof list);
    __GDCurrentDL = &object;
    J3DTevStage stage;
    stage.setStageNo(1);
    stage.mTevColorOp = 0x34, stage.mTevColorAB = 0x56, stage.mTevColorCD = 0x78;
    stage.mTevAlphaOp = 0x9A, stage.mTevAlphaAB = 0xBC, stage.mTevSwapModeInfo = 0xDE;
    stage.load(0);
    auto* gpu = new smg::GxCommandProcessor({[](std::uint32_t) -> const std::uint8_t* { return nullptr; }, [] {},
                                             [](std::uint16_t, bool) {}, [](const char*, std::uint32_t) { std::exit(1); },
                                             [](std::uint8_t, std::uint8_t, std::uint16_t, const std::uint8_t*) {}, [](std::uint32_t) {}});
    gpu->put(list, static_cast<std::uint32_t>(object.ptr - list));
    check(gpu->bp[0xC2] == 0x345678 && gpu->bp[0xC3] == 0x9ABCDE, "a TEV stage's BP commands name its registers, as on the Wii");
    delete gpu;
}

// A display list's vertex counts are the GPU's big-endian halves: a strip of 3 two-byte vertices, then a fan of 1.
void vertexCounts() {
    static const std::uint8_t list[] = {0x98, 0x00, 0x03, 1, 2, 3, 4, 5, 6, 0xA0, 0x00, 0x01, 7, 8, 0x00};
    J3DShapeDraw draw(list, sizeof list);
    check(draw.countVertex(2) == 4, "a display list's vertex counts read as the GPU reads them");
}

// The premise of the Mario adapts: gcc on ARM packs bitfields from bit 0, where Metrowerks began at bit 31.
void bitfieldOrder() {
    union { struct { unsigned first : 1, second : 1, rest : 30; } bits; u32 word; } flags = {};
    flags.bits.second = 1;
    check(flags.word == 2, "gcc on ARM packs a word's bitfields from its lowest bit");
}
} // namespace

int runColorTests() {
    packing();
    tevStage();
    vertexCounts();
    bitfieldOrder();
    std::printf("%u colour, TEV and display list word checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runColorTests(); }
#endif
