#include <smg/Scene.hpp>
#include <Game/AreaObj/AreaForm.hpp>
#include <Game/AreaObj/AreaObj.hpp>
#include <Game/AreaObj/AreaObjFollower.hpp>
#include <Game/AreaObj/LightArea.hpp>
#include <Game/AreaObj/LightAreaHolder.hpp>
#include <Game/Map/ActorAppearSwitchListener.hpp>
#include <Game/Map/LightDataHolder.hpp>
#include <Game/Map/LightFunction.hpp>
#include <Game/Map/LightZoneDataHolder.hpp>
#include <Game/Map/SleepController.hpp>
#include <Game/Map/SleepControllerHolder.hpp>
#include <Game/Map/StageSwitch.hpp>
#include <Game/Util/AreaObjUtil.hpp>
#include <Game/Util/BaseMatrixFollowTargetHolder.hpp>
#include <Game/Util/Functor.hpp>
#include <Game/Util/JMapUtil.hpp>
#include <Game/Util/ObjUtil.hpp>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

namespace {
// NameObjFactory's light areas and view group cubes; no other area is brought up yet.
struct AreaKind {
    const char* name;
    int form;
    bool light; // a LightArea, else a plain AreaObj
};
const AreaKind areaKinds[]{{"LightCtrlCube", AreaForm::Type_Cube2, true}, {"LightCtrlCylinder", AreaForm::Type_Cylinder, true},
                           {"ViewGroupCtrlCube", AreaForm::Type_Cube2, false}};

[[noreturn]] void stop(const char* what, const char* name) {
    std::fprintf(stderr, "NativeScene: %s%s is not brought up\n", what, name);
    std::abort();
}
} // namespace

namespace smg {
struct NativeScene::AppearWatch {
    const StageSwitchCtrl* switches;
    MR::FunctorBase* on;
    MR::FunctorBase* off;
    bool wasOn;
};

// SleepControlFunc::addSleepControl's listener for an area: awake while the switch is on.
struct NativeScene::AreaSleep : SwitchEventListener {
    AreaObj* area;
    SleepController* controller = nullptr;
    explicit AreaSleep(AreaObj* a) : area(a) {}
    void listenSwitchOnEvent() override { area->awake(); }
    void listenSwitchOffEvent() override { area->sleep(); }
};

bool NativeScene::placeAreaObj(const JMapInfoIter& row) {
    const char* name = nullptr;
    if (!MR::getObjectName(&name, row)) return false;
    for (const AreaKind& kind : areaKinds) {
        if (std::strcmp(kind.name, name)) continue;
        // A linked area stands relative to its map part's moving matrix, which only the part's actor has.
        if (MR::isValidFollowID(row)) {
            mSkippedAreas.push_back(std::string(name) + " linked to a map part");
            return false;
        }
        AreaObj* area = kind.light ? new LightArea(kind.form, name) : new AreaObj(kind.form, name);
        area->init(row);
        mAreas.push_back(area);
        return true;
    }
    mSkippedAreas.emplace_back(name);
    return false;
}

// AreaObjContainer's manager for them, of 0x40 areas.
AreaObjMgr* NativeScene::viewCubes() {
    if (!mViewCubes) mViewCubes = new AreaObjMgr(0x40, "ViewGroupCtrlCube");
    return mViewCubes;
}

LightAreaHolder* NativeScene::lightAreas(bool create) {
    // AreaObjContainer makes its managers with the scene; the holder registers itself with LightFunction.
    if (!mLightAreas && create) new LightAreaHolder(0x80, "LightArea");
    return mLightAreas;
}

void NativeScene::watchSwitchAppear(const StageSwitchCtrl* switches, const MR::FunctorBase& on, const MR::FunctorBase& off) {
    mAppearWatches.push_back(new AppearWatch{switches, on.clone(nullptr), off.clone(nullptr), false});
}

void NativeScene::addAreaSleep(AreaObj* area, const JMapInfoIter& row) {
    if (!MR::isExistStageSwitchSleep(row)) return;
    AreaSleep* sleep = new AreaSleep(area);
    sleep->controller = new SleepController(row, sleep);
    mAreaSleeps.push_back(sleep);
}

void NativeScene::syncAreaSleeps() {
    for (AreaSleep* sleep : mAreaSleeps) sleep->controller->initSync();
}

// SleepControllerHolder, then SwitchWatcherHolder calling the area's validate or invalidate on a change.
void NativeScene::moveAreas() {
    for (AreaSleep* sleep : mAreaSleeps) sleep->controller->update();
    for (AppearWatch* watch : mAppearWatches) {
        const bool on = watch->switches->isOnSwitchAppear();
        if (on != watch->wasOn) (*(on ? watch->on : watch->off))();
        watch->wasOn = on;
    }
}

const AreaLightInfo* NativeScene::areaLightAt(const TVec3f& position) {
    if (!mLightData) return nullptr;
    ZoneLightID id;
    LightFunction::tryFindNewAreaLightID(position, &id);
    return mLightData->findAreaLight(mLightZones->getAreaLightNameInZoneData(id));
}

// Areas, their forms and switch controls, and the watches: none has a destructor the game calls.
void NativeScene::forgetAreas() {
    for (AreaObj* area : mAreas) {
        switch (area->mFormType) {
        case AreaForm::Type_Cube1: case AreaForm::Type_Cube2: delete static_cast<AreaFormCube*>(area->mForm); break;
        case AreaForm::Type_Sphere: delete static_cast<AreaFormSphere*>(area->mForm); break;
        case AreaForm::Type_Bowl: delete static_cast<AreaFormBowl*>(area->mForm); break;
        case AreaForm::Type_Cylinder: delete static_cast<AreaFormCylinder*>(area->mForm); break;
        }
        forgetSwitches(area->mSwitchCtrl);
        delete area;
    }
    for (AppearWatch* watch : mAppearWatches) {
        ::operator delete(watch->on);
        ::operator delete(watch->off);
        delete watch;
    }
    for (AreaSleep* sleep : mAreaSleeps) {
        forgetSwitchId(sleep->controller->mSwitchIdInfo);
        delete sleep->controller;
        delete sleep;
    }
    delete mLightAreas;
    delete mViewCubes;
    mAreas.clear(), mAppearWatches.clear(), mAreaSleeps.clear();
    mLightAreas = nullptr;
    mViewCubes = nullptr;
}
} // namespace smg

namespace MR {
// Followers are not brought up; placeAreaObj leaves linked areas out before they get here.
bool addBaseMatrixFollowerAreaObj(AreaObj*, const JMapInfoIter& rIter) {
    if (!isValidFollowID(rIter)) return false;
    stop("an area follower", "");
}

AreaObjMgr* getAreaObjManager(const char* pName) {
    smg::NativeScene& scene = smg::NativeScene::current();
    if (!std::strcmp(pName, "LightArea")) return scene.lightAreas(true);
    if (!std::strcmp(pName, "ViewGroupCtrlCube")) return scene.viewCubes();
    stop("the area manager ", pName);
}

void listenNameObjStageSwitchOnOffAppear(const NameObj*, const StageSwitchCtrl* pSwitches, const MR::FunctorBase& rOn,
                                         const MR::FunctorBase& rOff) {
    smg::NativeScene::current().watchSwitchAppear(pSwitches, rOn, rOff);
}
} // namespace MR

void SleepControlFunc::addSleepControl(AreaObj* pObj, const JMapInfoIter& rIter) {
    smg::NativeScene::current().addAreaSleep(pObj, rIter);
}

void LightFunction::registerLightAreaHolder(LightAreaHolder* pHolder) {
    smg::NativeScene::current().registerLightAreas(pHolder);
}

bool LightFunction::tryFindNewAreaLightID(const TVec3f& rPos, ZoneLightID* pId) {
    return smg::NativeScene::current().lightAreas(true)->tryFindLightID(rPos, pId);
}
