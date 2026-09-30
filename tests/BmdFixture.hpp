#pragma once
// A small J3D model as the disc stores it (big-endian blocks), for the model and J3D tests.
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using Data = std::vector<std::uint8_t>;
void put16(Data& b, std::size_t at, unsigned v) { b[at] = v >> 8; b[at + 1] = v; }
void put32(Data& b, std::size_t at, std::uint32_t v) { put16(b, at, v >> 16); put16(b, at + 2, v); }
void putFloat(Data& b, std::size_t at, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put32(b, at, bits);
}
Data block(const char* name, unsigned size) {
    Data b(size, 0);
    std::memcpy(b.data(), name, 4);
    put32(b, 4, size);
    return b;
}

struct Fixture {
    Data data;
    std::size_t shape, vertex, material, texture;
};

struct Options {
    unsigned command = 0x98;
    bool twoJoints = false;   // joint 1, a child of joint 0, moved by (10, 0, 0) and turned 90 degrees about Y
    bool envelope = false;    // the shape uses an even blend of both joints instead
};

// One shape and one material drawing a unit quad with one primitive command.
Fixture fixture(Options options = {}) {
    const unsigned joints = options.twoJoints || options.envelope ? 2 : 1;
    auto inf = block("INF1", 64);
    put32(inf, 20, 24);
    std::vector<std::pair<unsigned, unsigned>> hierarchy{{0x10, 0}, {1, 0}};
    if (joints == 2) hierarchy.insert(hierarchy.end(), {{0x10, 1}, {1, 0}});
    hierarchy.insert(hierarchy.end(), {{0x11, 0}, {1, 0}, {0x12, 0}, {2, 0}, {2, 0}});
    if (joints == 2) hierarchy.push_back({2, 0});
    hierarchy.push_back({0, 0});
    inf.resize(24 + hierarchy.size() * 4);
    put32(inf, 4, inf.size());
    for (std::size_t i = 0; i < hierarchy.size(); ++i) {
        put16(inf, 24 + i * 4, hierarchy[i].first);
        put16(inf, 26 + i * 4, hierarchy[i].second);
    }

    auto vtx = block("VTX1", 144);
    put32(vtx, 8, 64);
    put32(vtx, 12, 96);
    put32(vtx, 64, 9);
    put32(vtx, 68, 1);
    put32(vtx, 72, 4);
    put32(vtx, 80, 255);
    const float positions[]{-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    for (unsigned i = 0; i < 12; ++i) putFloat(vtx, 96 + i * 4, positions[i]);

    // As on the disc, DRW1 ends with its envelope entries again, which J3D's loader counts off.
    auto drw = block("DRW1", 32);
    const unsigned drawCount = options.envelope ? 2 : joints;
    put16(drw, 8, drawCount);
    put32(drw, 12, 20);
    put32(drw, 16, 24);
    if (options.envelope) drw[20] = drw[21] = 1;
    else if (joints == 2) put16(drw, 26, 1);

    auto jnt = block("JNT1", 24 + joints * 64 + 4);
    put16(jnt, 8, joints);
    put32(jnt, 12, 24);
    put32(jnt, 16, 24 + joints * 64);
    for (unsigned j = 0; j < joints; ++j) {
        for (unsigned i = 0; i < 3; ++i) putFloat(jnt, 24 + j * 64 + 4 + i * 4, 1);
        put16(jnt, 24 + joints * 64 + j * 2, j);
    }
    if (joints == 2) {
        put16(jnt, 24 + 64 + 18, 0x4000);
        putFloat(jnt, 24 + 64 + 24, 10);
    }

    auto shp = block("SHP1", 131);
    put16(shp, 8, 1);
    put32(shp, 12, 44);
    put32(shp, 16, 84);
    put32(shp, 24, 86);
    put32(shp, 28, 102);
    put32(shp, 32, 104);
    put32(shp, 36, 115);
    put32(shp, 40, 123);
    put16(shp, 46, 1);
    put32(shp, 86, 9);
    put32(shp, 90, 3);
    put32(shp, 94, 255);
    const unsigned count = options.command == 0x90 ? 3 : 4;
    shp[104] = options.command;
    put16(shp, 105, count);
    for (unsigned i = 0; i < count; ++i) put16(shp, 107 + i * 2, i);
    if (options.command == 0xa0 || options.command == 0x80) {
        put16(shp, 111, 3);
        put16(shp, 113, 2);
    }
    // The shape's current matrix: the child joint when there is one.
    put16(shp, 115, joints == 2 && !options.envelope ? 1 : 0);
    put16(shp, 117, 1);
    put16(shp, 102, joints == 2 && !options.envelope ? 1 : 0);
    put32(shp, 123, 3 + count * 2);

    auto mat = block("MAT3", 475);
    put16(mat, 8, 1);
    put32(mat, 12, 132);
    put32(mat, 16, 464);
    put32(mat, 28, 466);
    put32(mat, 32, 470);
    put32(mat, 88, 474);
    put16(mat, 132 + 72, 65535);
    put16(mat, 132 + 132, 65535);
    put32(mat, 466, 2);
    put32(mat, 470, 0xffffffff);
    mat[474] = 1;

    auto tex = block("TEX1", 20);
    put32(tex, 12, 20);

    Fixture f;
    f.data.assign(32, 0);
    std::memcpy(f.data.data(), "J3D2bmd3", 8);
    put32(f.data, 12, options.envelope ? 8 : 7);
    const auto append = [&](const Data& b) {
        const auto offset = f.data.size();
        f.data.insert(f.data.end(), b.begin(), b.end());
        return offset;
    };
    append(inf);
    f.vertex = append(vtx);
    if (options.envelope) {
        // Envelope 0 mixes joints 0 and 1 half and half, with identity inverse binds.
        auto evp = block("EVP1", 28 + 4 + 4 + 8 + 96);
        put16(evp, 8, 1);
        put32(evp, 12, 28);
        put32(evp, 16, 32);
        put32(evp, 20, 36);
        put32(evp, 24, 44);
        evp[28] = 2;
        put16(evp, 32, 0);
        put16(evp, 34, 1);
        putFloat(evp, 36, 0.5f);
        putFloat(evp, 40, 0.5f);
        for (unsigned j = 0; j < 2; ++j) for (unsigned d = 0; d < 3; ++d) putFloat(evp, 44 + j * 48 + d * 20, 1);
        append(evp);
    }
    append(drw);
    append(jnt);
    f.shape = append(shp);
    f.material = append(mat);
    f.texture = append(tex);
    put32(f.data, 8, f.data.size());
    return f;
}

} // namespace
