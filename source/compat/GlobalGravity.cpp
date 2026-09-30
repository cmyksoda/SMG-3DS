#include <smg/Scene.hpp>
#include <Game/Gravity/GravityCreator.hpp>
#include <Game/Gravity/WireGravity.hpp>
#include <Game/LiveActor/RailRider.hpp>
#include <Game/Util/JMapInfo.hpp>
#include <Game/Util/JMapUtil.hpp>
#include <cstring>
#include <new>
#include <type_traits>

namespace {
template <class Creator> GravityCreator* create() { return new Creator; }

// Neither creators nor gravities have virtual destructors, so each goes as the type it was made.
template <class Creator> void destroy(GravityCreator* base) {
    auto* creator = static_cast<Creator*>(base);
    using Gravity = std::remove_pointer_t<decltype(creator->mGravityInstance)>;
    if (Gravity* gravity = creator->mGravityInstance) {
        gravity->~Gravity();
        ::operator delete(gravity);
    }
    creator->~Creator();
    ::operator delete(creator);
}

struct Kind {
    const char* name;
    GravityCreator* (*create)();
    void (*destroy)(GravityCreator*);
};

// The wire creator keeps the RailRider it sampled, which the game leaves to the scene heap.
void destroyWire(GravityCreator* base) {
    if (RailRider* rider = static_cast<WireGravityCreator*>(base)->mRailRider) {
        BezierRail* rail = rider->mBezierRail;
        for (int i = 0; i < rail->mNumRailParts; ++i) {
            delete rail->mRailParts[i].mRailPartLinear;
            delete rail->mRailParts[i].mRailPartBezier;
        }
        delete[] rail->mRailParts;
        delete[] rail->mPointCoords;
        delete rail->mIter;
        delete rail;
        delete rider;
    }
    destroy<WireGravityCreator>(base);
}

// NameObjFactory's global gravities.
const Kind kinds[]{
    {"GlobalCubeGravity", create<CubeGravityCreator>, destroy<CubeGravityCreator>},
    {"GlobalConeGravity", create<ConeGravityCreator>, destroy<ConeGravityCreator>},
    {"GlobalDiskGravity", create<DiskGravityCreator>, destroy<DiskGravityCreator>},
    {"GlobalDiskTorusGravity", create<DiskTorusGravityCreator>, destroy<DiskTorusGravityCreator>},
    {"GlobalPlaneGravity", create<PlaneGravityCreator>, destroy<PlaneGravityCreator>},
    {"GlobalPlaneGravityInBox", create<PlaneInBoxGravityCreator>, destroy<PlaneInBoxGravityCreator>},
    {"GlobalPlaneGravityInCylinder", create<PlaneInCylinderGravityCreator>, destroy<PlaneInCylinderGravityCreator>},
    {"GlobalPointGravity", create<PointGravityCreator>, destroy<PointGravityCreator>},
    {"GlobalSegmentGravity", create<SegmentGravityCreator>, destroy<SegmentGravityCreator>},
    {"GlobalWireGravity", create<WireGravityCreator>, destroyWire},
};

bool used(const JMapInfoIter& row, const char* field) {
    s32 id = -1;
    return row.getValue(field, &id) && id != -1;
}
} // namespace

namespace smg {
bool NativeScene::placeGlobalGravity(const JMapInfoIter& row) {
    const char* name = nullptr;
    if (!MR::getObjectName(&name, row)) return false;
    const Kind* kind = nullptr;
    for (const Kind& candidate : kinds) {
        if (std::strcmp(candidate.name, name) == 0) kind = &candidate;
    }
    if (!kind) {
        mSkippedGravities.emplace_back(name);
        return false;
    }

    // GlobalGravityObj::init: the creator builds, sets up and registers the gravity.
    GravityCreator* creator = kind->create();
    creator->createFromJMap(row);
    PlanetGravity* gravity = creator->getGravity();
    mGravities.push_back({creator, kind->destroy, gravity, name, zoneOf(row)});

    // makeActorAppeared, then updateSwitch: switch A gates it on and B off, and every switch starts off.
    gravity->mAppeared = true;
    const bool switchA = used(row, "SW_A"), switchB = used(row, "SW_B");
    if (switchA || switchB) gravity->mActivated = !switchA;
    if (switchA || switchB || used(row, "SW_SLEEP") || used(row, "FollowId")) mStartStateGravities.emplace_back(name);
    return true;
}
} // namespace smg
