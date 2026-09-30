#include <smg/Scene.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/NameObj/NameObjHolder.hpp>
#include <Game/NameObj/NameObjRegister.hpp>
#include <Game/Scene/SceneNameObjMovementController.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <Game/Util/JMapInfo.hpp>
#include <Game/Util/SceneUtil.hpp>
#include <Game/Util/SingletonHolder.hpp>
#include <Game/Util/SystemUtil.hpp>
#include <cstdio>
#include <cstdlib>

namespace {
smg::NativeScene* sCurrent;
}

namespace smg {
// GameSystemSceneController's capacity.
NativeScene::NativeScene() : mNameObjHolder(new NameObjHolder(0x1300)), mSceneObjHolder(new SceneObjHolder) {
    if (sCurrent) {
        std::fputs("NativeScene: a scene is already running\n", stderr);
        std::abort();
    }
    SingletonHolder<NameObjRegister>::init();
    SingletonHolder<NameObjRegister>::get()->setCurrentHolder(mNameObjHolder);
    sCurrent = this;
    // GameScene::init creates these first; the rest of its list is not brought up.
    MR::createSceneObj(SceneObj_PlanetGravityManager);
}

NativeScene::~NativeScene() {
    // The game frees the whole scene heap at once; here each scene object goes by itself.
    for (const PlacedGravity& placed : mGravities) placed.destroy(placed.creator);
    for (int id = 0; id < SceneObj_NumMax; ++id) delete mSceneObjHolder->getObj(id);
    delete mSceneObjHolder;
    SingletonHolder<NameObjRegister>::get()->setCurrentHolder(nullptr);
    delete mNameObjHolder;
    sCurrent = nullptr;
}

NativeScene& NativeScene::current() {
    if (!sCurrent) {
        std::fputs("NativeScene: the game asked for its scene before one was created\n", stderr);
        std::abort();
    }
    return *sCurrent;
}

int NativeScene::addZone(const TPos3f& placement) {
    mZones.push_back(placement);
    return static_cast<int>(mZones.size()) - 1;
}

void NativeScene::addZoneTable(int zone, const JMapInfo& table) {
    mZoneTables.emplace_back(table.mData, zone);
}

int NativeScene::zoneOf(const JMapInfoIter& row) const {
    for (const auto& [table, zone] : mZoneTables) {
        if (row.mInfo && row.mInfo->mData == table) return zone;
    }
    std::fputs("NativeScene: a placement row came from a table no zone registered\n", stderr);
    std::abort();
}

void NativeScene::movement() {
    if (mSyncRequested) {
        MR::syncWithFlagsAllSceneNameObj();
        mSyncRequested = false;
    }
}
} // namespace smg

NameObjHolder* getSceneNameObjHolder() {
    return &smg::NativeScene::current().nameObjHolder();
}

// The game makes about 120 kinds of scene object here; the port makes the ones it has brought up.
NameObj* SceneObjHolder::newEachObj(int id) {
    switch (id) {
    case SceneObj_PlanetGravityManager:
        return new PlanetGravityManager("重力");
    }
    std::fprintf(stderr, "SceneObjHolder: scene object %d is not brought up\n", id);
    std::abort();
}

namespace MR {
SceneObjHolder* getSceneObjHolder() {
    return &smg::NativeScene::current().sceneObjHolder();
}

// The game also answers false before its scene controller exists.
bool isExistSceneObj(int id) {
    return sCurrent && sCurrent->sceneObjHolder().isExist(id);
}

// StageDataHolder counts the zones it placed, and finds a row's zone by the table it came from.
bool isPlacementLocalStage() {
    return smg::NativeScene::current().hasPlacedZones();
}

const TPos3f* getZonePlacementMtx(const JMapInfoIter& rIter) {
    const smg::NativeScene& scene = smg::NativeScene::current();
    return &scene.zonePlacement(scene.zoneOf(rIter));
}

// SceneNameObjMovementController's version asks for the same sync on its next movement().
void notifyRequestNameObjMovementOnOff() {
    smg::NativeScene::current().requestMovementSync();
}
} // namespace MR
