#include "KclFixture.hpp"
#include "KclRayOracle.hpp"
#include <smg/Bcsv.hpp>
#include <smg/Kcl.hpp>
#include <smg/Scene.hpp>
#include <Game/Camera/CameraPolygonCodeUtil.hpp>
#include <Game/Map/CollisionCode.hpp>
#include <Game/Map/CollisionParts.hpp>
#include <Game/Map/HitInfo.hpp>
#include <Game/Map/KCollision.hpp>
#include <Game/Util/TriangleFilter.hpp>
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

std::uint32_t word(const std::uint8_t* p) {
    std::uint32_t value;
    std::memcpy(&value, p, sizeof value);
    return value;
}

// The file nativeKcl turns in place keeps the Wii's layout; each section holds what the repacked file's does.
void inPlaceMatchesRepacked(bool twoLevels) {
    const auto raw = floorKcl(twoLevels);
    smg::Kcl kcl;
    std::string error;
    check(kcl.load({raw.data(), raw.size()}, error), "the floor repacks");
    std::vector<std::uint8_t> native(raw.begin(), raw.end());
    check(!smg::nativeKcl(native.data(), native.size()), "a Wii KCL turns native in place");
    const auto* repacked = static_cast<const std::uint8_t*>(kcl.data());
    const auto& file = *reinterpret_cast<const KCLFile*>(repacked);
    const std::uint32_t positions = word(&native[0]), normals = word(&native[4]), prisms = word(&native[8]), octree = word(&native[12]);
    check(positions == 0x38 && octree < native.size(), "its offsets read natively");
    check(!std::memcmp(&native[positions], repacked + file.mPosOffset, normals - positions) &&
              !std::memcmp(&native[normals], repacked + file.mNormOffset, prisms + 16 - normals),
          "its positions and normals are the repacked ones");
    check(!std::memcmp(&native[prisms + 16], repacked + file.mPrismOffset + 16, octree - prisms - 16), "and its prisms");
    check(!std::memcmp(&native[octree], repacked + file.mOctreeOffset, native.size() - octree), "and its octree, node by node");
    float thickness;
    std::memcpy(&thickness, &native[0x10], sizeof thickness);
    check(thickness == file.mThickness && static_cast<s32>(word(&native[0x20])) == file.mXMask &&
              static_cast<s32>(word(&native[0x2C])) == file.mBlockWidthShift && static_cast<s32>(word(&native[0x34])) == file.mBlockXYShift,
          "and its header's values");
    std::vector<std::uint8_t> cut(raw.begin(), raw.begin() + static_cast<long>(octree));
    const std::vector<std::uint8_t> before = cut;
    check(smg::nativeKcl(cut.data(), cut.size()) && cut == before, "a cut-off file is refused and left as it was");
}

// inPlace: the game's own path, the file turned native where its archive holds it (32-bit pointers only).
void floorCollision(bool twoLevels, bool inPlace = false) {
    const auto raw = floorKcl(twoLevels), table = attributes();
    smg::Kcl kcl;
    smg::Bcsv pa;
    std::string error;
    check(kcl.load({raw.data(), raw.size()}, error) && kcl.triangles() == 2, "a Wii KCL repacks");
    check(pa.load({table.data(), table.size()}, error), "its attribute table loads");
    std::vector<std::uint64_t> held((raw.size() + 7) / 8);
    void* data = kcl.data();
    if (inPlace) {
        std::memcpy(held.data(), raw.data(), raw.size());
        check(!smg::nativeKcl(reinterpret_cast<std::uint8_t*>(held.data()), raw.size()), "a Wii KCL turns native in place");
        data = held.data();
    }

    smg::NativeScene scene;
    Server server;
    server.init(data, pa.info().mData);
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
    again.init(data, pa.info().mData);
    check(again.mFile->mPos == positions && down(again, 30, 20), "a second part shares the image unchanged");
}

// What one cast wrote, over a fill pattern so a stray or a missing write shows.
struct Cast {
    KC_PrismData* returned = nullptr;
    u32 count = 0xA5A5A5A5u;
    f32 dists[34];
    u8 flags[34];
    KC_PrismData* prisms[34];

    Cast() {
        std::memset(dists, 0xA5, sizeof dists);
        std::memset(flags, 0xA5, sizeof flags);
        std::memset(prisms, 0xA5, sizeof prisms);
    }

    bool operator==(const Cast& other) const {
        return returned == other.returned && count == other.count && !std::memcmp(dists, other.dists, sizeof dists) &&
               !std::memcmp(flags, other.flags, sizeof flags) && !std::memcmp(prisms, other.prisms, sizeof prisms);
    }
};

Cast castFirst(const Server& server, const TVec3f& from, const TVec3f& direction, bool petariCode) {
    Cast cast;
    cast.returned = petariCode ? petari::checkArrow(server, from, direction, cast.dists, cast.flags, nullptr, nullptr, 0)
                               : server.checkArrow(from, direction, cast.dists, cast.flags, nullptr, nullptr, 0);
    return cast;
}

Cast castCollecting(const Server& server, const TVec3f& from, const TVec3f& direction, u32 maxHits, bool petariCode) {
    Cast cast;
    cast.returned = petariCode ? petari::checkArrow(server, from, direction, cast.dists, cast.flags, &cast.count, cast.prisms, maxHits)
                               : server.checkArrow(from, direction, cast.dists, cast.flags, &cast.count, cast.prisms, maxHits);
    return cast;
}

struct Random {
    std::uint32_t state = 2463534242u;
    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float unit() { return static_cast<float>(next() >> 8) / 16777216.0f; }
    float range(float low, float high) { return low + (high - low) * unit(); }
    template <typename T> T pick(std::initializer_list<T> values) { return *(values.begin() + next() % values.size()); }
};

// A ray of one of the kinds that matter to the walk: anywhere, onto corners and edges, along an axis, on a block's border, tiny, along
// the shared diagonal.
void aRay(Random& random, int kind, TVec3f& from, TVec3f& direction) {
    switch (kind % 6) {
    case 0:
        from.set(random.range(-20, 270), random.range(-20, 270), random.range(-20, 270));
        direction.set(random.range(-1, 1), random.range(-1, 1), random.range(-1, 1));
        direction.scale(random.range(0, 500));
        break;
    case 1: {
        const float height = random.range(1, 100);
        from.set(random.pick<float>({0, 30, 50, 70, 100}), height, random.pick<float>({0, 30, 50, 70, 100}));
        direction.set(0, -(height + random.range(-3, 60)), 0);
        break;
    }
    case 2: {
        from.set(random.range(-20, 270), random.range(-20, 270), random.range(-20, 270));
        const float length = random.range(-300, 300);
        const int axis = random.next() % 3;
        direction.set(axis == 0 ? length : 0, axis == 1 ? length : 0, axis == 2 ? length : 0);
        break;
    }
    case 3:
        from.set(random.pick<float>({-10, 118, 246}), random.range(-20, 270), random.pick<float>({-10, 118, 246, 50}));
        direction.set(random.pick<float>({0, 0, 40}), random.pick<float>({0, -80, 80}), random.pick<float>({0, 90, -90}));
        break;
    case 4:
        from.set(random.range(0, 100), random.range(0, 5), random.range(0, 100));
        direction.set(random.pick<float>({0, 1e-5f, -2e-4f, 4e-4f}), random.pick<float>({0, -1e-3f, -0.5f, -3.0f}), random.pick<float>({0, 1e-4f}));
        break;
    default: {
        const float along = random.range(0, 100);
        from.set(along, random.range(1, 60), 100 - along);
        direction.set(0, -random.range(20, 120), 0);
    }
    }
}

// The port's ray cast against Petari's, over the fixture's floor (both octree shapes): every output equal to the bit.
void raysMatchPetari(bool twoLevels) {
    const auto raw = floorKcl(twoLevels);
    smg::Kcl kcl;
    std::string error;
    check(kcl.load({raw.data(), raw.size()}, error), "the floor repacks for the ray test");
    Server server;
    server.init(kcl.data(), nullptr);
    Random random;
    unsigned hits = 0, repeated = 0;

    for (int i = 0; i < 2000; ++i) {
        TVec3f from, direction;
        aRay(random, i, from, direction);
        const Cast first = castFirst(server, from, direction, true);
        check(castFirst(server, from, direction, false) == first, "a first-hit cast equals Petari's");
        hits += first.returned != nullptr;

        for (u32 maxHits : {32u, 3u, 2u, 1u}) {
            const Cast collecting = castCollecting(server, from, direction, maxHits, true);
            check(castCollecting(server, from, direction, maxHits, false) == collecting, "a collecting cast equals Petari's");
            repeated += collecting.count > 2;
        }
    }

    check(hits > 300, "enough of the rays meet the floor to mean something");
    check(!twoLevels || repeated > 100, "and many list a triangle again from the next block");
}

// A world-space strike through a placed part: its hits equal what Petari's cast and Triangle::fillData give, filter or none.
// Filter 1 rejects every second hit; filter 2 every hit of the floor's first triangle, so a later hit takes the slot.
struct Filter : TriangleFilterBase {
    int kind;
    mutable int calls = 0;
    explicit Filter(int filterKind) : kind(filterKind) {}
    bool isInvalidTriangle(const Triangle* triangle) const override {
        ++calls;
        return kind == 1 ? calls % 2 == 0 : triangle->mIdx == 0;
    }
};

u32 petariStrike(CollisionParts& part, HitInfo* hits, u32 maxHits, const TVec3f& from, const TVec3f& direction, const TriangleFilterBase* filter) {
    f32 distances[64];
    KC_PrismData* prisms[64];
    u8 classifications[64];
    const f32 length = direction.length();
    TVec3f localFrom, localTo;
    part.mInvBaseMatrix.mult(from, localFrom);
    part.mInvBaseMatrix.mult(from + direction, localTo);
    const TVec3f localDirection = localTo - localFrom;
    u32 count = 0;
    petari::checkArrow(*part.mServer, localFrom, localDirection, distances, classifications, &count, prisms, maxHits);
    u32 kept = 0;

    for (u32 i = 0; i < count; i++) {
        HitInfo& hit = hits[kept];
        TVec3f position(localFrom + localDirection * distances[i]);
        part.mBaseMatrix.mult(position, position);
        hit.mParentTriangle.fillData(&part, part.mServer->toIndex(prisms[i]), part.mHitSensor);

        if (filter != nullptr && filter->isInvalidTriangle(&hit.mParentTriangle)) {
            continue;
        }

        hit._60 = length * distances[i];
        hit.mHitPos = position;
        kept++;
    }

    return kept;
}

bool sameHit(const HitInfo& a, const HitInfo& b) {
    const Triangle& x = a.mParentTriangle;
    const Triangle& y = b.mParentTriangle;
    return x.mParts == y.mParts && x.mIdx == y.mIdx && x.mSensor == y.mSensor && !std::memcmp(x.mNormals, y.mNormals, sizeof x.mNormals) &&
           !std::memcmp(x.mPos, y.mPos, sizeof x.mPos) && !std::memcmp(&a._60, &b._60, sizeof a._60) &&
           !std::memcmp(&a.mHitPos, &b.mHitPos, sizeof a.mHitPos);
}

void partStrikesMatchPetari() {
    const auto raw = floorKcl(true);
    smg::Kcl kcl;
    std::string error;
    check(kcl.load({raw.data(), raw.size()}, error), "the floor repacks for the part test");
    CollisionParts* part = new CollisionParts();
    part->mServer->setData(kcl.data());
    // Turned about Y and moved, as a placed part is; the inverse as CollisionParts' constructor makes it.
    const float c = std::cos(0.7f), s = std::sin(0.7f);
    const float base[3][4]{{c, 0, s, 30}, {0, 1, 0, -20}, {-s, 0, c, 5}};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 4; ++k) part->mBaseMatrix.mMtx[r][k] = base[r][k];
    PSMTXInverse(part->mBaseMatrix.toMtxPtr(), part->mInvBaseMatrix.toMtxPtr());
    Random random;
    unsigned repeated = 0;

    for (int i = 0; i < 800; ++i) {
        TVec3f from, direction;
        aRay(random, i, from, direction);
        TVec3f worldFrom, worldTo;
        part->mBaseMatrix.mult(from, worldFrom);
        part->mBaseMatrix.mult(from + direction, worldTo);
        const TVec3f worldDirection = worldTo - worldFrom;

        for (int kind = 0; kind < 3; ++kind) {
            const Filter expectedFilter(kind), actualFilter(kind);
            HitInfo expected[32], actual[32];
            const u32 maxHits = random.pick<u32>({32, 32, 5, 1});
            const u32 count = petariStrike(*part, expected, maxHits, worldFrom, worldDirection, kind ? &expectedFilter : nullptr);
            check(part->checkStrikeLine(actual, maxHits, worldFrom, worldDirection, kind ? &actualFilter : nullptr) == count,
                  "a part's strike finds as many hits as Petari's code");
            for (u32 k = 0; k < count; ++k) check(sameHit(actual[k], expected[k]), "and each hit's triangle, distance and position are Petari's, to the bit");
            check(actualFilter.calls == expectedFilter.calls, "and the triangle filter is asked as often");
            repeated += count > 2;
        }
    }

    check(repeated > 100, "many strikes meet a triangle again in the next block");
    delete part->mServer->mapInfo;
    delete part->mServer;
    delete part;
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
    inPlaceMatchesRepacked(false);
    inPlaceMatchesRepacked(true);
    if (sizeof(void*) == 4) floorCollision(false, true), floorCollision(true, true);
    raysMatchPetari(false);
    raysMatchPetari(true);
    partStrikesMatchPetari();
    rejected();
    std::printf("%u original collision checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runCollisionTests(); }
#endif
