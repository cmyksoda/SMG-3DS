// The two scene hooks the scene library's extracts call (tools/prepare-petari.py), answered for the whole game as its
// own code answers them; native builds link the native scene's answers instead (source/compat/Scene.cpp).
#include <Game/Map/CollisionDirector.hpp>
#include <Game/Player/MarioActor.hpp>
#include <Game/Player/MarioHolder.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <Game/System/GameSystem.hpp>
#include <Game/System/GameSystemSceneController.hpp>
#include <Game/Scene/Scene.hpp>
#include <Game/LiveActor/Spine.hpp>
#include <Game/Util/SingletonHolder.hpp>
#include <revolution/os.h>
#include <Game/Util/PlayerUtil.hpp>

NameObjHolder* getSceneNameObjHolder() {
    return SingletonHolder< GameSystem >::get()->mSceneController->mObjHolder;
}

// Whether this calcAnim's view matrices will be drawn (GameSystem::frameLoop, adapted; ModelManager::calcView).
extern "C" {
bool smgViewNeeded = true;
}

CollisionCode* getSceneCollisionCode() {
    return MR::getCollisionDirector()->mCode;
}

extern "C" BOOL smgDevWarp(float* to, int* seconds);  // source/compat/Kpad.cpp

// dev.warp: once a run, the given seconds after Mario first appears (smgFramePhase calls this every logic step).
extern "C" void smgDevWarpStep() {
    static int steps = 0;
    float to[3];
    int seconds = 0;
    if (steps < 0 || !smgDevWarp(to, &seconds) || !MR::isExistSceneObj(SceneObj_MarioHolder)) return;
    if (!MR::getSceneObj< MarioHolder >(SceneObj_MarioHolder)->getMarioActor() || ++steps < seconds * 60) return;
    MR::setPlayerPos(TVec3f(to[0], to[1], to[2]));
    OSReport("dev.warp: Mario to (%.0f, %.0f, %.0f)\n", to[0], to[1], to[2]);
    steps = -1;
}

// Where the game's state machines are, every few seconds, for runs that no longer stop: the nerves are static objects
// that tools/heartbeat-names.sh names from the ELF. Called from the game thread every 300th logic step (smgFramePhase).
extern "C" void smgHeartbeat() {
    GameSystem* system = SingletonHolder< GameSystem >::get();
    if (!system || !system->mSpine) return;
    GameSystemSceneController* controller = system->mSceneController;
    const Nerve* scene = controller && controller->mScene && controller->mScene->mSpine ? controller->mScene->mSpine->getCurrentNerve() : nullptr;
    OSReport("heartbeat: system %p controller %p scene \"%s\" %p\n", system->mSpine->getCurrentNerve(),
             controller && controller->mSpine ? controller->mSpine->getCurrentNerve() : nullptr,
             controller ? controller->mCurrSceneControlInfo.mScene : "", scene);
    // Where Mario is, for planning a run's route through a stage.
    if (MR::isExistSceneObj(SceneObj_MarioHolder)) {
        const MarioActor* mario = MR::getSceneObj< MarioHolder >(SceneObj_MarioHolder)->getMarioActor();
        if (mario) OSReport("heartbeat: Mario at (%.0f, %.0f, %.0f)\n", mario->mPosition.x, mario->mPosition.y, mario->mPosition.z);
    }
}
