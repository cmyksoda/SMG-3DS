#pragma once
// A small Wii KCL floor and its attribute table, as the disc stores them, for the collision and actor tests.
#include <Game/Map/CollisionCode.hpp>
#include <JSystem/JGadget/hashcode.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
struct Writer {
    std::vector<std::uint8_t> bytes;
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(v >> (24 - 8 * i)); }
    void u16(std::uint16_t v) { bytes.push_back(v >> 8), bytes.push_back(v & 255); }
    void f32(float v) { std::uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void at(std::size_t offset, std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes[offset + i] = v >> (24 - 8 * i); }
};

struct V { float x, y, z; };
V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V unit(V a) { const float l = std::sqrt(dot(a, a)); return {a.x / l, a.y / l, a.z / l}; }
V outward(V n, V edge, V away) {
    V e = unit(cross(n, edge));
    return dot(e, away) > 0 ? V{-e.x, -e.y, -e.z} : e;
}

// The floor square (0..100, 0, 0..100) as two up-facing triangles.
const V floorTriangles[2][3]{{{0, 0, 0}, {0, 0, 100}, {100, 0, 0}}, {{100, 0, 100}, {100, 0, 0}, {0, 0, 100}}};

// A KCL as the disc stores it. The octree is one leaf (twoLevels false) or a 2x2x2 grid of branches that
// all share one block of eight leaves, each listing both triangles.
std::vector<std::uint8_t> floorKcl(bool twoLevels) {
    Writer w;
    w.bytes.resize(0x38);
    const std::size_t positions = w.bytes.size();
    for (const auto& t : floorTriangles) w.f32(t[0].x), w.f32(t[0].y), w.f32(t[0].z);
    const std::size_t normals = w.bytes.size();
    std::vector<float> heights;
    for (const auto& t : floorTriangles) {
        const V n = unit(cross(sub(t[1], t[0]), sub(t[2], t[0])));
        const V e0 = outward(n, sub(t[2], t[0]), sub(t[1], t[0]));
        const V e1 = outward(n, sub(t[1], t[0]), sub(t[2], t[0]));
        const V e2 = outward(n, sub(t[2], t[1]), sub(t[0], t[1]));
        for (const V& v : {n, e0, e1, e2}) w.f32(v.x), w.f32(v.y), w.f32(v.z);
        heights.push_back(dot(sub(t[1], t[0]), e2));
    }
    const std::size_t prisms = w.bytes.size() - 16; // slot zero overlaps the last normal, as on the disc
    for (int i = 0; i < 2; ++i) {
        w.f32(heights[i]);
        w.u16(i), w.u16(i * 4), w.u16(i * 4 + 1), w.u16(i * 4 + 2), w.u16(i * 4 + 3), w.u16(i);
    }
    const std::size_t octree = w.bytes.size();
    // As on the disc, a list's offset points one u16 early, into the last node before it.
    if (twoLevels) {
        for (int node = 0; node < 8; ++node) w.u32(32);                  // branches to the block at +32
        for (int leaf = 0; leaf < 8; ++leaf) w.u32(0x80000000u | 30);    // lists at +30 from that block
    } else {
        w.u32(0x80000000u | 2);
    }
    w.u16(1), w.u16(2), w.u16(0);
    w.at(0, positions), w.at(4, normals), w.at(8, prisms), w.at(12, octree);
    Writer tail;
    tail.f32(40);                                     // thickness
    tail.f32(-10), tail.f32(-10), tail.f32(-10);      // minimum
    const std::uint32_t mask = twoLevels ? 0xffffff00u : 0xffffff80u;
    tail.u32(mask), tail.u32(mask), tail.u32(mask);
    tail.u32(7);                                      // block width 128
    tail.u32(twoLevels ? 1 : 0xffffffffu), tail.u32(twoLevels ? 2 : 0xffffffffu);
    std::memcpy(w.bytes.data() + 0x10, tail.bytes.data(), tail.bytes.size());
    return w.bytes;
}

// The .pa attribute table: triangle 0 is a death floor seen by camera 7, triangle 1 normal with camera 9.
std::vector<std::uint8_t> attributes() {
    Writer w;
    const char* names[]{"camera_id", "Floor_code"};
    w.u32(2), w.u32(2), w.u32(16 + 2 * 12), w.u32(8);
    for (unsigned f = 0; f < 2; ++f) {
        w.u32(JGadget::getHashCode(names[f])), w.u32(0xffffffff);
        w.u16(f * 4), w.bytes.push_back(0), w.bytes.push_back(0);
    }
    w.u32(7), w.u32(CollisionFloorCode_Death);
    w.u32(9), w.u32(CollisionFloorCode_Normal);
    return w.bytes;
}

} // namespace
