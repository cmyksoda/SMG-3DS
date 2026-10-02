#include <smg/Animation.hpp>
#include <smg/CameraAnim.hpp>
#include <smg/Mesh.hpp>
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

// A BCK for two joints: joint 0 turns about Y from 0 to a quarter turn over 10 frames (rotations stored at
// half precision, shift 1, the first key negative); joint 1 holds a translation of 100 along X.
std::vector<std::uint8_t> turnBck() {
    Writer w;
    for (const char* c = "J3D1bck1"; *c; ++c) w.u8(*c);
    w.u32(0), w.u32(1);
    while (w.bytes.size() < 0x20) w.u8(0);
    const std::size_t block = w.bytes.size();
    w.u32(0x414e4b31), w.u32(0);
    w.u8(2), w.u8(1), w.u16(10);            // loop, rotation shift 1, 10 frames
    w.u16(2), w.u16(1), w.u16(7), w.u16(2); // joints; scale, rotation and translation values
    for (int i = 0; i < 4; ++i) w.u32(0);
    const auto here = [&] { return static_cast<std::uint32_t>(w.bytes.size() - block); };
    w.at32(block + 0x14, here());
    // Per joint X, Y, Z: (scale, rotation, translation), each (key count, first value, key type).
    const std::uint16_t tables[2][3][9]{
        {{1, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 2, 1, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0, 0}},
        {{1, 0, 0, 0, 0, 0, 1, 1, 0}, {1, 0, 0, 0, 0, 0, 1, 0, 0}, {1, 0, 0, 0, 0, 0, 1, 0, 0}}};
    for (const auto& joint : tables)
        for (const auto& axis : joint)
            for (auto v : axis) w.u16(v);
    w.pad4();
    w.at32(block + 0x18, here());
    w.u32(0x3f800000);                      // scale 1
    w.at32(block + 0x1c, here());
    // X holds 0; Y keys (frame, value, tangent): (0, -2, 0) and (10, 8192, 0).
    for (std::int16_t v : {std::int16_t(0), std::int16_t(0), std::int16_t(-2), std::int16_t(0), std::int16_t(10),
                           std::int16_t(8192), std::int16_t(0)})
        w.u16(static_cast<std::uint16_t>(v));
    w.pad4();
    w.at32(block + 0x20, here());
    w.u32(0), w.u32(0x42c80000);            // translations 0 and 100
    w.at32(block + 4, static_cast<std::uint32_t>(w.bytes.size() - block));
    w.at32(8, static_cast<std::uint32_t>(w.bytes.size()));
    return w.bytes;
}

void jointAnimation() {
    const auto bytes = turnBck();
    smg::JointAnimation bck;
    std::string error;
    check(bck.load({bytes.data(), bytes.size()}, error), "a BCK loads");
    check(bck.joints() == 2 && bck.key().mFrameMax == 10, "with its joints and length");
    std::vector<smg::JointTransform> pose(2);
    bck.pose(0, pose);
    check(pose[0].rotation[1] == -4 && pose[1].translation[0] == 100, "keys before the first frame, shifted; a held translation");
    bck.pose(10, pose);
    check(pose[0].rotation[1] == 16384, "a quarter turn at the last key");

    // Two joints, the child 100 along X; one vertex rigid on the child, one halfway between both.
    smg::Model model;
    model.joints.resize(2);
    model.joints[1].parent = 0;
    model.joints[1].translation[0] = 100;
    model.skin.draws = {{false, 1, {{1, 1.0f}}}, {true, 0, {{0, 0.5f}, {1, 0.5f}}}};
    model.skin.inverseBind = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}, {1, 0, 0, -100, 0, 1, 0, 0, 0, 0, 1, 0}};
    model.skin.rest.resize(2);
    model.skin.rest[0].position[0] = 10;   // 10 past the child, in its own space
    model.skin.rest[1].position[0] = 50;   // halfway, in model space
    model.skin.matrix = {0, 1};
    std::vector<std::array<float, 12>> world;
    std::vector<smg::JointTransform> local(2);
    local[1].translation[0] = 100;
    smg::poseSkeleton(model, local, world);
    std::vector<smg::ModelVertex> skinned;
    smg::skinModel(model, world, skinned);
    check(near(skinned[0].position[0] / 255, 110.0f / 255) && near(skinned[1].position[0] / 255, 50.0f / 255),
          "at the bind pose each vertex stays put");
    local[0].rotation[1] = 16384; // the root turns a quarter about Y: +X goes to -Z
    smg::poseSkeleton(model, local, world);
    smg::skinModel(model, world, skinned);
    check(std::fabs(skinned[0].position[2] + 110) < 0.01f && std::fabs(skinned[0].position[0]) < 0.01f,
          "a rigid vertex turns with its joint");
    check(std::fabs(skinned[1].position[2] + 50) < 0.01f, "a weighted vertex blends its joints");

    // Two triangles alike at the bind pose but on different draw matrices must not share packed vertices.
    model.vertices.assign(6, {});
    model.skin.rest.assign(6, {});
    model.skin.matrix = {0, 0, 0, 1, 1, 1};
    for (int v = 0; v < 6; ++v) model.vertices[v].position[0] = model.skin.rest[v].position[0] = float(v % 3);
    model.shapes = {{0, 6, 0, 3}};
    std::vector<std::uint32_t> origin;
    auto mesh = smg::packMesh(model, &origin);
    check(mesh.vertices.size() == 6 && origin.size() == 6 && origin[3] == 3, "vertices on other matrices stay apart");
    model.skin.matrix.clear();
    mesh = smg::packMesh(model);
    check(mesh.vertices.size() == 3, "without a skin, alike vertices are shared");
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
// A keyed camera animation as the disc holds one (CKAN, big-endian), turned native and read as CameraAnim reads it.
void cameraAnimation() {
    std::vector<std::uint8_t> file(0x20 + 0x60 + 4 + 7 * 4);
    const auto put = [&](std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) file[at + i] = std::uint8_t(v >> (24 - 8 * i)); };
    std::memcpy(file.data(), "ANDOCKAN", 8);
    put(0x08, 1), put(0x0C, 0), put(0x10, 1), put(0x14, 4), put(0x18, 479), put(0x1C, 0x60);
    put(0x20, 2), put(0x24, 0), put(0x28, 0);  // position x: two keys of (time, value, slope)
    for (int c = 1; c < 8; ++c) put(0x20 + c * 12, 1), put(0x20 + c * 12 + 4, 6);
    put(0x80, 7 * 4);
    const float values[7] = {0, 100.5f, 0, 60, -20.25f, 0, 45};
    for (int i = 0; i < 7; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &values[i], 4);
        put(0x84 + i * 4, bits);
    }
    auto bad = file;
    check(!smg::nativeCameraAnimation(file.data(), file.size()), "a keyed camera animation turns");
    const auto word = [&](std::size_t at) { std::uint32_t v; std::memcpy(&v, &file[at], 4); return v; };
    const auto value = [&](int i) { float v; std::memcpy(&v, &file[0x84 + i * 4], 4); return v; };
    check(word(0x18) == 479 && word(0x1C) == 0x60 && word(0x80) == 28, "camera header and value count native");
    check(word(0x20) == 2 && word(0x20 + 7 * 12 + 4) == 6, "camera component counts and offsets native");
    check(value(1) == 100.5f && value(4) == -20.25f && value(6) == 45, "camera values native");
    check(!std::memcmp(file.data(), "ANDOCKAN", 8), "camera magic and kind stay as text");
    bad[0x80 + 3] = 0x40;  // more values than the file holds
    check(smg::nativeCameraAnimation(bad.data(), bad.size()) != nullptr, "camera values past the file are refused");
}
} // namespace

int runAnimationTests() {
    pipeColors();
    jointAnimation();
    rejected();
    cameraAnimation();
    std::printf("%u J3D animation checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runAnimationTests(); }
#endif
