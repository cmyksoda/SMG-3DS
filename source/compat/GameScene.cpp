// The two scene hooks the scene library's extracts call (tools/prepare-petari.py), answered for the whole game as its
// own code answers them; native builds link the native scene's answers instead (source/compat/Scene.cpp).
#include <Game/Map/CollisionDirector.hpp>
#include <Game/System/GameSystem.hpp>
#include <Game/System/GameSystemSceneController.hpp>
#include <Game/Scene/Scene.hpp>
#include <Game/LiveActor/Spine.hpp>
#include <Game/Util/SingletonHolder.hpp>
#include <revolution/os.h>

NameObjHolder* getSceneNameObjHolder() {
    return SingletonHolder< GameSystem >::get()->mSceneController->mObjHolder;
}

CollisionCode* getSceneCollisionCode() {
    return MR::getCollisionDirector()->mCode;
}

// Where the game's state machines are, every few seconds, for runs that no longer stop: the nerves are static objects
// that tools/heartbeat-names.sh names from the ELF. Called from the display copy, every 300th frame.
extern "C" void smgHeartbeat() {
    GameSystem* system = SingletonHolder< GameSystem >::get();
    if (!system || !system->mSpine) return;
    GameSystemSceneController* controller = system->mSceneController;
    const Nerve* scene = controller && controller->mScene && controller->mScene->mSpine ? controller->mScene->mSpine->getCurrentNerve() : nullptr;
    OSReport("heartbeat: system %p controller %p scene \"%s\" %p\n", system->mSpine->getCurrentNerve(),
             controller && controller->mSpine ? controller->mSpine->getCurrentNerve() : nullptr,
             controller ? controller->mCurrSceneControlInfo.mScene : "", scene);
}
