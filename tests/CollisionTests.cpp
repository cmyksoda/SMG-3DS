#include <smg/Bcsv.hpp>
#include <smg/Kcl.hpp>
#include <smg/Scene.hpp>
#include <Game/Camera/CameraPolygonCodeUtil.hpp>
#include <Game/Map/CollisionCode.hpp>
#include <Game/Map/KCollision.hpp>
#include <JSystem/JGadget/hashcode.hpp>
#include <cmath>
#include <cstdint>
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
bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

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

// KCollisionServer has no destructor; the game frees the scene heap whole.
struct Server : KCollisionServer {
    ~Server() { delete mapInfo; }
};

KC_PrismData* down(const Server& server, float x, float z, float from = 50, float length = 100, float* fraction = nullptr) {
    f32 distance = -1;
    u8 flag = 0;
    KC_PrismData* prism = server.checkArrow(TVec3f(x, from, z), TVec3f(0.0f, -length, 0.0f), &distance, &flag, nullptr, nullptr, 0);
    if (fraction) *fraction = distance;
    return prism;
}

void floorCollision(bool twoLevels) {
    const auto raw = floorKcl(twoLevels), table = attributes();
    smg::Kcl kcl;
    smg::Bcsv pa;
    std::string error;
    check(kcl.load({raw.data(), raw.size()}, error) && kcl.triangles() == 2, "a Wii KCL repacks");
    check(pa.load({table.data(), table.size()}, error), "its attribute table loads");

    smg::NativeScene scene;
    Server server;
    server.init(kcl.data(), pa.info().mData);
    // As CollisionParts::init does for a part.
    MR::initCameraCodeCollection("floor", 0);
    check(server.calcFarthestVertexDistance(), "every triangle has attributes");
    MR::termCameraCodeCollection();
    check(scene.cameraCodes().size() == 1 && scene.cameraCodes()[0].codes == std::vector<u32>{7, 9},
          "the triangles' camera codes are collected for the camera");
    check(near(server.mMaxVertexDistance, std::sqrt(2.0f) * 100), "the farthest vertex is the square's far corner");

    for (int t = 0; t < 2; ++t) {
        for (int k = 0; k < 3; ++k) {
            const TVec3f p = server.getPos(server.getPrismData(t), k);
            const V& e = floorTriangles[t][k];
            check(near(p.x, e.x) && near(p.y, e.y) && near(p.z, e.z), "a prism's corners are its triangle's");
        }
    }

    float fraction = 0;
    KC_PrismData* hit = down(server, 30, 20, 50, 100, &fraction);
    check(hit && server.toIndex(hit) == 0 && near(fraction, 0.5f), "a ray down meets the floor halfway");
    CollisionCode& code = scene.collisionCode();
    check(code.getFloorCode(server.getAttributes(server.toIndex(hit))) == CollisionFloorCode_Death, "and reads its floor code");
    hit = down(server, 80, 80);
    check(hit && server.toIndex(hit) == 1 && code.getFloorCode(server.getAttributes(1)) == CollisionFloorCode_Normal,
          "the other triangle has its own attributes");
    check(!down(server, 30, 20, -50, -100), "a ray from below meets the floor's back and passes");
    check(!down(server, 150, 20), "a ray beside the floor misses");
    check(!down(server, 30, 20, 50, 40), "a ray that ends above the floor misses");
    if (twoLevels) {
        check(down(server, 30, 20, 200, 250, &fraction) && near(fraction, 200 / 250.0f), "a ray through two blocks meets it");
    }

    Fxyz below;
    below.x = 30, below.y = -5, below.z = 20;
    f32 depth = 0;
    check(server.checkPoint(&below, 1.0f, &depth) && near(depth, 5), "a point under the floor is inside it");
    below.y = 5;
    check(!server.checkPoint(&below, 1.0f, &depth), "a point above is not");

    // Another placement of the same model attaches to the same image, which is already set up.
    TVec3f* positions = server.mFile->mPos;
    Server again;
    again.init(kcl.data(), pa.info().mData);
    check(again.mFile->mPos == positions && down(again, 30, 20), "a second part shares the image unchanged");
}

void rejected() {
    std::string error;
    smg::Kcl kcl;
    auto raw = floorKcl(true);
    check(!kcl.load({raw.data(), 0x37}, error), "a truncated header is rejected");
    auto bad = raw;
    Writer w{bad};
    w.at(12, raw.size() + 4);
    check(!kcl.load({w.bytes.data(), w.bytes.size()}, error), "an octree past the end is rejected");
    const std::size_t octree = (std::size_t(raw[12]) << 24) | (raw[13] << 16) | (raw[14] << 8) | raw[15];
    w.bytes = raw;
    w.at(octree, 0x40000000u);
    check(!kcl.load({w.bytes.data(), w.bytes.size()}, error), "a branch outside the file is rejected");
    w.bytes = raw;
    w.at(octree + 32, 0x80000000u | 31);
    check(!kcl.load({w.bytes.data(), w.bytes.size()}, error), "a list at an odd offset is rejected");
    w.bytes = raw;
    w.bytes[octree + 64 + 3] = 3;
    check(!kcl.load({w.bytes.data(), w.bytes.size()}, error), "a list naming a missing triangle is rejected");
    raw = floorKcl(false);
    w.bytes = raw;
    const std::size_t single = (std::size_t(raw[12]) << 24) | (raw[13] << 16) | (raw[14] << 8) | raw[15];
    w.at(0x2c, 0), w.at(single, 4);
    check(!kcl.load({w.bytes.data(), w.bytes.size()}, error), "a branch below the smallest block is rejected");
    check(kcl.load({raw.data(), raw.size()}, error), "and the file itself is fine");
}
} // namespace

int runCollisionTests() {
    floorCollision(false);
    floorCollision(true);
    rejected();
    std::printf("%u original collision checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runCollisionTests(); }
#endif
