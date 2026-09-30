// The two scene hooks the scene library's extracts call (tools/prepare-petari.py), answered for the whole game as its
// own code answers them; native builds link the native scene's answers instead (source/compat/Scene.cpp).
#include <Game/Map/CollisionDirector.hpp>
#include <Game/System/GameSystem.hpp>
#include <Game/System/GameSystemSceneController.hpp>
#include <Game/Util/SingletonHolder.hpp>

NameObjHolder* getSceneNameObjHolder() {
    return SingletonHolder< GameSystem >::get()->mSceneController->mObjHolder;
}

CollisionCode* getSceneCollisionCode() {
    return MR::getCollisionDirector()->mCode;
}
