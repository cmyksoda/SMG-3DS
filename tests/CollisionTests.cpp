#include "KclFixture.hpp"
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
