// The actor side of the native scene: placement's end and the teardown that frees what the game's scene heap
// would. Entry points into systems not brought up are in GameBoundaries.cpp. See docs/PORTING.md, "Road to Mario".
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

namespace MR {
// The scene's init state: the port places everything before its first movement.
bool isInitializeStatePlacementSomething() { return false; }
} // namespace MR

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
