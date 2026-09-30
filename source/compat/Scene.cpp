#include <smg/Scene.hpp>
#include <Game/NameObj/NameObjHolder.hpp>
#include <Game/NameObj/NameObjRegister.hpp>
#include <Game/Scene/SceneNameObjMovementController.hpp>
#include <Game/Util/SingletonHolder.hpp>
#include <Game/Util/SystemUtil.hpp>
#include <cstdio>
#include <cstdlib>

namespace {
smg::NativeScene* sCurrent;
}

namespace smg {
// GameSystemSceneController's capacity.
NativeScene::NativeScene() : mNameObjHolder(new NameObjHolder(0x1300)) {
    if (sCurrent) {
        std::fputs("NativeScene: a scene is already running\n", stderr);
        std::abort();
    }
    SingletonHolder<NameObjRegister>::init();
    SingletonHolder<NameObjRegister>::get()->setCurrentHolder(mNameObjHolder);
    sCurrent = this;
}

NativeScene::~NativeScene() {
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

namespace MR {
// SceneNameObjMovementController's version asks for the same sync on its next movement().
void notifyRequestNameObjMovementOnOff() {
    smg::NativeScene::current().requestMovementSync();
}
} // namespace MR
