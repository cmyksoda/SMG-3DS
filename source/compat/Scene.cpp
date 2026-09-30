#include <smg/Scene.hpp>
#include <smg/Bcsv.hpp>
#include <Game/Camera/CameraPolygonCodeUtil.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/Map/CollisionCode.hpp>
#include <Game/Map/StageSwitch.hpp>
#include <Game/NameObj/NameObjHolder.hpp>
#include <Game/NameObj/NameObjRegister.hpp>
#include <Game/Scene/SceneNameObjMovementController.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <Game/Util/JMapInfo.hpp>
#include <Game/Util/JMapUtil.hpp>
#include <Game/Util/SceneUtil.hpp>
#include <Game/Util/SingletonHolder.hpp>
#include <Game/Util/SystemUtil.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
smg::NativeScene* sCurrent;
}

namespace smg {
// GameSystemSceneController's capacity.
NativeScene::NativeScene()
    : mNameObjHolder(new NameObjHolder(0x1300)), mSceneObjHolder(new SceneObjHolder), mCollisionCode(new CollisionCode) {
    if (sCurrent) {
        std::fputs("NativeScene: a scene is already running\n", stderr);
        std::abort();
    }
    SingletonHolder<NameObjRegister>::init();
    SingletonHolder<NameObjRegister>::get()->setCurrentHolder(mNameObjHolder);
    sCurrent = this;
    // GameScene::init creates these first; the rest of its list is not brought up.
    MR::createSceneObj(SceneObj_PlanetGravityManager);
    // SceneFunction makes the switch container with the scene's other placement holders.
    MR::createSceneObj(SceneObj_StageSwitchContainer);
}

NativeScene::~NativeScene() {
    // The game frees the whole scene heap at once; here each scene object goes by itself.
    for (const PlacedGravity& placed : mGravities) forgetGravity(placed);
    forgetAreas();
    forgetLights();
    // The zone switches, which the game leaves to the scene heap with the container.
    if (auto* switches = static_cast<StageSwitchContainer*>(mSceneObjHolder->getObj(SceneObj_StageSwitchContainer))) {
        for (int i = 0; i < switches->mSwitches.size(); ++i) delete switches->mSwitches[i].mSwitch;
        delete switches->mGlobalSwitches;
    }
    for (int id = 0; id < SceneObj_NumMax; ++id) delete mSceneObjHolder->getObj(id);
    delete mSceneObjHolder;
    for (CodeTable* table : {mCollisionCode->mFloorTable, mCollisionCode->mWallTable, mCollisionCode->mSoundTable,
                             mCollisionCode->mCameraTable}) {
        delete[] table->mHashTable;
        delete[] table->mCodeTable;
        delete[] table->mNameTable;
        delete table;
    }
    delete mCollisionCode;
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

int NativeScene::addZone(const std::string& name, const TPos3f& placement) {
    mZones.push_back(placement);
    mZoneNames.push_back(name);
    return static_cast<int>(mZones.size()) - 1;
}

void NativeScene::addZoneTable(int zone, const JMapInfo& table) {
    mZoneTables.emplace_back(table.mData, zone);
}

void NativeScene::addZonePaths(int zone, const JMapInfo& paths, std::vector<const JMapInfo*> points) {
    mZonePaths.push_back({zone, &paths, std::move(points)});
}

const NativeScene::ZonePaths* NativeScene::zonePaths(int zone) const {
    for (const ZonePaths& paths : mZonePaths)
        if (paths.zone == zone) return &paths;
    return nullptr;
}

int NativeScene::zoneOf(const JMapInfoIter& row) const {
    for (const auto& [table, zone] : mZoneTables) {
        if (row.mInfo && row.mInfo->mData == table) return zone;
    }
    std::fputs("NativeScene: a placement row came from a table no zone registered\n", stderr);
    std::abort();
}

void NativeScene::beginCameraCodes(const char* host, s32 zone) {
    mCameraCodes.push_back({host, zone, {}});
    mCollectingCameraCodes = true;
}

void NativeScene::addCameraCode(u32 code) {
    if (!mCollectingCameraCodes) {
        std::fputs("NativeScene: a camera code arrived outside a collection\n", stderr);
        std::abort();
    }
    auto& codes = mCameraCodes.back().codes;
    if (std::find(codes.begin(), codes.end(), code) == codes.end()) codes.push_back(code);
}

void NativeScene::endCameraCodes() {
    mCollectingCameraCodes = false;
}

void NativeScene::movement() {
    if (mSyncRequested) {
        MR::syncWithFlagsAllSceneNameObj();
        mSyncRequested = false;
    }
    moveAreas();
    moveGravities();
}
} // namespace smg

NameObjHolder* getSceneNameObjHolder() {
    return &smg::NativeScene::current().nameObjHolder();
}

CollisionCode* getSceneCollisionCode() {
    return &smg::NativeScene::current().collisionCode();
}

// The game makes about 120 kinds of scene object here; the port makes the ones it has brought up.
NameObj* SceneObjHolder::newEachObj(int id) {
    switch (id) {
    case SceneObj_PlanetGravityManager:
        return new PlanetGravityManager("重力");
    case SceneObj_StageSwitchContainer:
        return new StageSwitchContainer();
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

s32 getPlacedZoneId(const JMapInfoIter& rIter) {
    return smg::NativeScene::current().zoneOf(rIter);
}

const TPos3f* getZonePlacementMtx(const JMapInfoIter& rIter) {
    const smg::NativeScene& scene = smg::NativeScene::current();
    return &scene.zonePlacement(scene.zoneOf(rIter));
}

// StageDataHolder::getCommonPathPointInfo, in the zone that placed the row: the rail whose l_id is the
// row's CommonPath_ID, and the point table named for that rail's row.
void getRailInfo(JMapInfoIter* pRailIter, const JMapInfo** ppPointInfo, const JMapInfoIter& rIter) {
    const smg::NativeScene& scene = smg::NativeScene::current();
    s32 railId = -1;
    getRailId(rIter, &railId);
    const smg::NativeScene::ZonePaths* paths = scene.zonePaths(scene.zoneOf(rIter));
    const JMapInfoIter rail = paths ? paths->paths->findElement<s32>("l_id", railId, 0) : JMapInfoIter();
    if (!paths || rail == paths->paths->end() || rail.mIndex >= static_cast<int>(paths->points.size()) ||
        !paths->points[rail.mIndex]) {
        std::fprintf(stderr, "NativeScene: rail %d is not in its zone's paths\n", railId);
        std::abort();
    }
    *pRailIter = JMapInfoIter(paths->paths, rail.mIndex);
    *ppPointInfo = paths->points[rail.mIndex];
}

// StageDataHolder's zone list: the galaxy is zone 0.
s32 getZoneNum() {
    return smg::NativeScene::current().zoneCount();
}

const char* getZoneNameFromZoneId(s32 zone) {
    return smg::NativeScene::current().zoneName(zone).c_str();
}

// CameraDirector collects the camera codes of each part's collision; the native scene keeps them for it.
void initCameraCodeCollection(const char* pName, s32 zone) {
    smg::NativeScene::current().beginCameraCodes(pName, zone);
}

void registerCameraCode(u32 code) {
    smg::NativeScene::current().addCameraCode(code);
}

void termCameraCodeCollection() {
    smg::NativeScene::current().endCameraCodes();
}

// SceneNameObjMovementController's version asks for the same sync on its next movement().
void notifyRequestNameObjMovementOnOff() {
    smg::NativeScene::current().requestMovementSync();
}
} // namespace MR
