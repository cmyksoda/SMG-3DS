#include <smg/Animation.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a - b) < 1.5f / 255; }

struct Writer {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) { u8(v >> 8), u8(v & 255); }
    void u32(std::uint32_t v) { u16(v >> 16), u16(v & 0xffff); }
    void at32(std::size_t o, std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes[o + i] = v >> (24 - 8 * i); }
    void pad4() { while (bytes.size() % 4) u8(0); }
};

// One color table for "Pipe" register C1: red eases 100 -> 200 over frames 0..10 (keys of three), green holds
// 50, blue has no keys, alpha holds 300 (past 255, which a TEV register may hold). One konst table: K0 red 20.
std::vector<std::uint8_t> pipeBrk() {
    Writer w;
    for (const char* c = "J3D1brk1"; *c; ++c) w.u8(*c);
    w.u32(0), w.u32(1);
    while (w.bytes.size() < 0x20) w.u8(0);
    const std::size_t block = w.bytes.size();
    w.u32(0x54524b31), w.u32(0);
    w.u8(0), w.u8(0xff), w.u16(10);  // attribute, pad, frame max
    w.u16(1), w.u16(1);               // one color table, one konst table
    const std::uint16_t counts[8]{6, 1, 0, 1, 1, 0, 0, 0};
    for (auto c : counts) w.u16(c);
    const std::size_t offsets = w.bytes.size();
    for (int i = 0; i < 14; ++i) w.u32(0);
    const auto here = [&] { return static_cast<std::uint32_t>(w.bytes.size() - block); };
    const auto table = [&](std::uint16_t r0, std::uint16_t r1, std::uint16_t g0, std::uint16_t g1, std::uint16_t a0,
                           std::uint16_t a1, std::uint8_t id) {
        w.u16(r0), w.u16(r1), w.u16(0);
        w.u16(g0), w.u16(g1), w.u16(0);
        w.u16(0), w.u16(0), w.u16(0);
        w.u16(a0), w.u16(a1), w.u16(0);
        w.u8(id), w.u8(0), w.u8(0), w.u8(0);
    };
    w.at32(offsets, here());
    table(2, 0, 1, 0, 1, 0, 1);
    w.at32(offsets + 4, here());
    table(1, 0, 0, 0, 0, 0, 0);
    for (int list = 0; list < 2; ++list) {
        w.at32(offsets + 8 + list * 4, here());
        w.u16(0), w.u16(0);
    }
    for (int list = 0; list < 2; ++list) {
        w.at32(offsets + 16 + list * 4, here());
        w.u16(1), w.u16(0xffff), w.u16(0), w.u16(8);
        for (const char* c = "Pipe"; *c; ++c) w.u8(*c);
        w.u8(0);
        w.pad4();
    }
    const std::vector<std::int16_t> values[8]{{0, 100, 0, 10, 200, 0}, {50}, {}, {300}, {20}, {}, {}, {}};
    for (int c = 0; c < 8; ++c) {
        w.at32(offsets + 24 + c * 4, here());
        for (auto v : values[c]) w.u16(static_cast<std::uint16_t>(v));
        w.pad4();
    }
    w.at32(block + 4, static_cast<std::uint32_t>(w.bytes.size() - block));
    w.at32(8, static_cast<std::uint32_t>(w.bytes.size()));
    return w.bytes;
}

void pipeColors() {
    const auto bytes = pipeBrk();
    smg::TevRegAnimation brk;
    std::string error;
    check(brk.load({bytes.data(), bytes.size()}, error), "a BRK loads");
    check(brk.key().mFrameMax == 10, "with its length");
    std::vector<smg::ModelMaterial> materials(2);
    materials[0].name = "Pipe";
    materials[1].name = "Other";
    materials[1].tevColors[1] = {0.5f, 0.5f, 0.5f, 0.5f};
    check(brk.apply(0, materials) == 2, "a color and a konst register of the named material");
    const auto& c1 = materials[0].tevColors[1];
    check(near(c1[0], 100 / 255.0f) && near(c1[1], 50 / 255.0f) && c1[2] == 0 && near(c1[3], 300 / 255.0f),
          "keys, a held value, no keys, and a value past 255");
    check(near(materials[0].konstColors[0][0], 20 / 255.0f), "the konst register");
    check(materials[1].tevColors[1][0] == 0.5f, "other materials are left alone");
    brk.apply(5, materials);
    check(near(materials[0].tevColors[1][0], 150 / 255.0f), "halfway between flat keys is halfway");
    brk.apply(20, materials);
    check(near(materials[0].tevColors[1][0], 200 / 255.0f), "past the last key it holds");
    brk.apply(2.5f, materials);
    // Hermite with flat tangents: 100 + 100 (3t^2 - 2t^3) at t = 0.25.
    check(near(materials[0].tevColors[1][0], (100 + 100 * (3 * 0.0625f - 2 * 0.015625f)) / 255.0f), "Hermite between keys");
}

void rejected() {
    auto bytes = pipeBrk();
    smg::TevRegAnimation brk;
    std::string error;
    check(!brk.load({bytes.data(), 0x30}, error), "a truncated file is rejected");
    auto bad = bytes;
    bad[0x20 + 0x10 + 1] = 5; // the red values shrink below what the keys need
    check(!brk.load({bad.data(), bad.size()}, error), "keys past their values are rejected");
    bad = bytes;
    bad[3] = '2';
    check(!brk.load({bad.data(), bad.size()}, error), "another J3D version is rejected");
}
} // namespace

int runAnimationTests() {
    pipeColors();
    rejected();
    std::printf("%u J3D animation checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runAnimationTests(); }
#endif
