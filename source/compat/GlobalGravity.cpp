#include <smg/Scene.hpp>
#include <Game/Gravity/GravityCreator.hpp>
#include <Game/Gravity/WireGravity.hpp>
#include <Game/LiveActor/RailRider.hpp>
#include <Game/Map/ActorAppearSwitchListener.hpp>
#include <Game/Map/SleepController.hpp>
#include <Game/AreaObj/LightAreaHolder.hpp>
#include <Game/Map/StageSwitch.hpp>
#include <Game/Util/BaseMatrixFollowTargetHolder.hpp>
#include <Game/Util/JMapIdInfo.hpp>
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
namespace {
// GlobalGravityObj::updateSwitch: switch A must be on and switch B off, for whichever the row uses.
void updateSwitch(PlanetGravity* gravity, const StageSwitchCtrl* switches) {
    if (!switches || !(switches->isValidSwitchA() || switches->isValidSwitchB())) return;
    bool switchA = true, switchB = true;
    if (switches->isValidSwitchA()) switchA = switches->isOnSwitchA();
    if (switches->isValidSwitchB()) switchB = !switches->isOnSwitchB();
    gravity->mActivated = switchA && switchB;
}
} // namespace

void NativeScene::forgetSwitchId(SwitchIdInfo* info) {
    if (!info) return;
    delete info->mIDInfo;
    delete info;
}

void NativeScene::forgetSwitches(StageSwitchCtrl* switches) {
    if (!switches) return;
    for (SwitchIdInfo* info : {switches->mSW_A, switches->mSW_B, switches->mSW_Appear, switches->mSW_Dead}) forgetSwitchId(info);
    delete switches;
}

// SleepControlFunc::addSleepControl's listener, calling what GlobalGravityObj's makeActorAppeared and
// makeActorDead do; SleepControllerHolder's own add, initSync and movement are the scene's.
struct NativeScene::GravitySleep : SwitchEventListener {
    PlanetGravity* gravity;
    const StageSwitchCtrl* switches;
    SleepController* controller = nullptr;
    GravitySleep(PlanetGravity* g, const StageSwitchCtrl* s) : gravity(g), switches(s) {}
    void listenSwitchOnEvent() override {
        gravity->mAppeared = true;
        updateSwitch(gravity, switches);
    }
    void listenSwitchOffEvent() override { gravity->mAppeared = false; }
};

void NativeScene::forgetGravity(const PlacedGravity& placed) {
    forgetSwitches(placed.switches);
    if (GravitySleep* sleep = placed.sleep) {
        forgetSwitchId(sleep->controller->mSwitchIdInfo);
        delete sleep->controller;
        delete sleep;
    }
    placed.destroy(placed.creator);
}

void NativeScene::syncAfterPlacement() {
    if (mLightAreas) mLightAreas->initAfterPlacement();
    syncAreaSleeps();
    for (const PlacedGravity& placed : mGravities)
        if (placed.sleep) placed.sleep->controller->initSync();
}

// SleepControllerHolder::movement, then GlobalGravityObj::movement for each gravity with switches. A dead
// actor is out of the scene's movement until it appears again.
void NativeScene::moveGravities() {
    for (const PlacedGravity& placed : mGravities)
        if (placed.sleep) placed.sleep->controller->update();
    for (const PlacedGravity& placed : mGravities)
        if (placed.gravity->mAppeared) updateSwitch(placed.gravity, placed.switches);
}

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

    // GlobalGravityObj::init: the creator builds, sets up and registers the gravity, the actor reads its
    // switches, appears (updating its switch state), and puts itself under sleep control.
    GravityCreator* creator = kind->create();
    creator->createFromJMap(row);
    PlanetGravity* gravity = creator->getGravity();
    StageSwitchCtrl* switches = nullptr;
    if (MR::isExistStageSwitchA(row) || MR::isExistStageSwitchB(row)) switches = MR::createStageSwitchCtrl(nullptr, row);
    gravity->mAppeared = true;
    updateSwitch(gravity, switches);
    GravitySleep* sleep = nullptr;
    if (MR::isExistStageSwitchSleep(row)) {
        sleep = new GravitySleep(gravity, switches);
        sleep->controller = new SleepController(row, sleep);
    }
    mGravities.push_back({creator, kind->destroy, gravity, name, zoneOf(row), switches, sleep});
    // MR::addBaseMatrixFollowerGravity: a row linked to a map part follows its matrix.
    if (MR::isValidFollowID(row)) mStartStateGravities.emplace_back(name);
    return true;
}
} // namespace smg
