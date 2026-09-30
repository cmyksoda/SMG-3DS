// The actor side of the native scene boundary. LiveActor reaches into systems the port has not brought up;
// each of those entry points stops the program by name if reached, and the scene frees what the game's
// scene heap would. See docs/PORTING.md, "Road to Mario".
#include <smg/Scene.hpp>
#include <Game/LiveActor/ActorAnimKeeper.hpp>
#include <Game/LiveActor/ActorLightCtrl.hpp>
#include <Game/LiveActor/ActorPadAndCameraCtrl.hpp>
#include <Game/LiveActor/AllLiveActorGroup.hpp>
#include <Game/LiveActor/Binder.hpp>
#include <Game/LiveActor/ClippingActorHolder.hpp>
#include <Game/LiveActor/ClippingActorInfo.hpp>
#include <Game/LiveActor/ClippingDirector.hpp>
#include <Game/LiveActor/ClippingGroupHolder.hpp>
#include <Game/LiveActor/ClippingJudge.hpp>
#include <Game/LiveActor/EffectKeeper.hpp>
#include <Game/LiveActor/HitSensor.hpp>
#include <Game/LiveActor/HitSensorInfo.hpp>
#include <Game/LiveActor/HitSensorKeeper.hpp>
#include <Game/LiveActor/LodCtrl.hpp>
#include <Game/LiveActor/LiveActor.hpp>
#include <Game/LiveActor/ModelManager.hpp>
#include <Game/LiveActor/SensorHitChecker.hpp>
#include <Game/LiveActor/ShadowController.hpp>
#include <Game/LiveActor/ViewGroupCtrl.hpp>
#include <Game/Map/CollisionCategorizedKeeper.hpp>
#include <Game/Map/CollisionCode.hpp>
#include <Game/Map/CollisionDirector.hpp>
#include <Game/Map/CollisionParts.hpp>
#include <Game/Map/KCollision.hpp>
#include <Game/Screen/StarPointerTarget.hpp>
#include <Game/Util/LiveActorUtil.hpp>
#include <Game/Util/MemoryUtil.hpp>
#include <Game/Util/PlayerUtil.hpp>
#include <Game/Util/ModelUtil.hpp>
#include <Game/Util/SceneUtil.hpp>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace {
[[noreturn]] void unported(const char* what) {
    std::fprintf(stderr, "Not brought up yet: %s\n", what);
    std::abort();
}
} // namespace

// Models and their animation: ModelManager, J3DModel and the animation keepers come with milestone 2.
ModelManager::ModelManager() { unported("ModelManager"); }
void ModelManager::init(const char*, const char*, bool) { unported("ModelManager::init"); }
void ModelManager::update() { unported("ModelManager::update"); }
void ModelManager::calcAnim() { unported("ModelManager::calcAnim"); }
void ModelManager::calcView() { unported("ModelManager::calcView"); }
ActorAnimKeeper* ActorAnimKeeper::tryCreate(LiveActor*) { unported("ActorAnimKeeper::tryCreate"); }
void ActorAnimKeeper::update() { unported("ActorAnimKeeper::update"); }
ActorPadAndCameraCtrl* ActorPadAndCameraCtrl::tryCreate(const ModelManager*, const TVec3f*) {
    unported("ActorPadAndCameraCtrl::tryCreate");
}
void ActorPadAndCameraCtrl::update() { unported("ActorPadAndCameraCtrl::update"); }
namespace MR {
J3DModel* getJ3DModel(const LiveActor*) { unported("MR::getJ3DModel"); }
void calcJ3DModel(LiveActor*) { unported("MR::calcJ3DModel"); }
ResourceHolder* getModelResourceHolder(const LiveActor*) { unported("MR::getModelResourceHolder"); }
// The actor's resources come with its model; the native scene hands map parts their collision directly.
CollisionParts* createCollisionPartsFromLiveActor(LiveActor*, const char*, HitSensor*, CollisionScaleType) {
    unported("MR::createCollisionPartsFromLiveActor");
}
CollisionParts* createCollisionPartsFromLiveActor(LiveActor*, const char*, HitSensor*, MtxPtr, CollisionScaleType) {
    unported("MR::createCollisionPartsFromLiveActor");
}
CollisionParts* createCollisionPartsFromResourceHolder(ResourceHolder*, const char*, HitSensor*, const TPos3f&, CollisionScaleType) {
    unported("MR::createCollisionPartsFromResourceHolder");
}
// JKRHeap's current heap: the port allocates from the ordinary heap.
JKRHeap* getCurrentHeap() { unported("MR::getCurrentHeap"); }
// ActorLightCtrl draws with the draw buffers; the preview lights models from the same tables itself.
void updateLightCtrl(LiveActor*) { unported("MR::updateLightCtrl"); }
// The scene's init state: the port places everything before its first movement.
bool isInitializeStatePlacementSomething() { return false; }
} // namespace MR
// Model LOD comes with models; view groups follow the player, who comes with Mario.
void LodCtrl::setViewCtrlPtr(const bool*, const bool*, const bool*, const bool*) { unported("LodCtrl::setViewCtrlPtr"); }
namespace MR {
TVec3f* getPlayerPos() { unported("MR::getPlayerPos"); }
} // namespace MR
ActorLightCtrl::ActorLightCtrl(const LiveActor*) { unported("ActorLightCtrl"); }
void ActorLightCtrl::reset() { unported("ActorLightCtrl::reset"); }

// Effects, shadows and the star pointer come with their directors; LiveActor's sound setup is left out of it.
EffectKeeper::EffectKeeper(const char*, ResourceHolder*, int, const char*) { unported("EffectKeeper"); }
void EffectKeeper::init(LiveActor*) { unported("EffectKeeper::init"); }
void EffectKeeper::setBinder(const Binder*) { unported("EffectKeeper::setBinder"); }
void EffectKeeper::update() { unported("EffectKeeper::update"); }
void EffectKeeper::stopEmitterOnClipped() { unported("EffectKeeper::stopEmitterOnClipped"); }
void EffectKeeper::playEmitterOffClipped() { unported("EffectKeeper::playEmitterOffClipped"); }
void EffectKeeper::clear() { unported("EffectKeeper::clear"); }
void EffectKeeper::enableSort() { unported("EffectKeeper::enableSort"); }
namespace MR {
// SoundUtil's version sets the object's floor code and plays its animation sounds; with no sound object it
// returns at once, and no actor has one until sound comes up.
void actorSoundMovement(LiveActor* pActor) {
    if (pActor->mSoundObject != nullptr) unported("MR::actorSoundMovement");
}
} // namespace MR
ShadowControllerList::ShadowControllerList(LiveActor*, u32) { unported("ShadowControllerList"); }
void ShadowControllerList::resetCalcCount() { unported("ShadowControllerList::resetCalcCount"); }
void ShadowControllerList::requestCalc() { unported("ShadowControllerList::requestCalc"); }
StarPointerTarget::StarPointerTarget(f32, const TVec3f*, MtxPtr, TVec3f) { unported("StarPointerTarget"); }

namespace smg {
namespace {
void forgetSensors(HitSensorKeeper* keeper) {
    if (!keeper) return;
    for (s32 i = 0; i < keeper->mSensorCount; ++i) {
        HitSensor* sensor = keeper->mSensorInfos[i]->mSensor;
        delete[] sensor->mSensors;
        delete sensor;
        delete keeper->mSensorInfos[i];
    }
    delete[] keeper->mSensorInfos;
    delete keeper;
}

void forgetCollisionParts(CollisionParts* parts) {
    if (!parts) return;
    delete parts->mServer->mapInfo;
    delete parts->mServer;
    delete parts;
}
} // namespace

void NativeScene::endActorPlacement() {
    MR::endInitLiveActorSystemInfo();
}

// The scene heap's part: the actors, then what the directors built for them.
void NativeScene::forgetActors() {
    if (auto* actors = static_cast<AllLiveActorGroup*>(mSceneObjHolder->getObj(SceneObj_AllLiveActorGroup))) {
        for (s32 i = 0; i < actors->getObjNum(); ++i) {
            auto* actor = static_cast<LiveActor*>(actors->getObj(i));
            forgetSensors(actor->mSensorKeeper);
            delete actor->mBinder;
            forgetCollisionParts(actor->mCollisionParts);
            delete actor;
        }
    }
    if (auto* clipping = static_cast<ClippingDirector*>(mSceneObjHolder->getObj(SceneObj_ClippingDirector))) {
        delete clipping->mJudge;
        ClippingActorHolder* holder = clipping->mActorHolder;
        // An info moves between the four lists; each is freed once.
        std::set<ClippingActorInfo*> infos;
        for (ClippingActorInfoList* list : {holder->_10, holder->_14, holder->_18, holder->_1C}) {
            for (s32 i = 0; i < list->_4; ++i) infos.insert(list->mClippingActorList[i]);
            delete[] list->mClippingActorList;
            delete list;
        }
        for (ClippingActorInfo* info : infos) delete info;
        delete[] holder->mViewGroupCtrl->mLodCtrls;
        delete[] holder->mViewGroupCtrl->mViewGroupData;
        delete holder->mViewGroupCtrl;
        delete holder;
        ClippingGroupHolder* groups = clipping->mGroupHolder;
        for (s32 i = 0; i < groups->mNumGroups; ++i) delete groups->mInfoGroups[i];
        delete[] groups->mInfoGroups;
        delete groups;
    }
    if (auto* sensors = static_cast<SensorHitChecker*>(mSceneObjHolder->getObj(SceneObj_SensorHitChecker))) {
        for (SensorGroup* group : {sensors->mPlayerGroup, sensors->mRideGroup, sensors->mEyeGroup, sensors->mSimpleGroup,
                                   sensors->mMapObjGroup, sensors->mCharacterGroup}) {
            delete[] group->mSensors;
            delete group;
        }
    }
    if (auto* collision = static_cast<CollisionDirector*>(mSceneObjHolder->getObj(SceneObj_CollisionDirector))) {
        constexpr s32 CategoryKeepers = 4; // CollisionDirector.cpp's CATEGORY_KEEPER_NUM
        for (s32 i = 0; i < CategoryKeepers; ++i) {
            CollisionCategorizedKeeper* keeper = collision->mCategoryKeeper[i];
            for (CollisionZone** zone = keeper->mZones.begin(); zone != keeper->mZones.end(); ++zone) delete *zone;
            delete[] keeper->mHitInfoArray;
            delete keeper;
        }
        delete[] collision->mCategoryKeeper;
        CollisionCode* code = collision->mCode;
        for (CodeTable* table : {code->mFloorTable, code->mWallTable, code->mSoundTable, code->mCameraTable}) {
            delete[] table->mHashTable;
            delete[] table->mCodeTable;
            delete[] table->mNameTable;
            delete table;
        }
        delete code;
    }
}
} // namespace smg
