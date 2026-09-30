#pragma once

class NameObjHolder;
class SceneObjHolder;

namespace smg {
// The scene state GameSystemSceneController owns in the game, held natively until GameSystem runs.
// Removable: see docs/PORTING.md, "The native scene boundary".
class NativeScene {
public:
    NativeScene();
    ~NativeScene();
    NativeScene(const NativeScene&) = delete;
    NativeScene& operator=(const NativeScene&) = delete;

    static NativeScene& current();
    NameObjHolder& nameObjHolder() { return *mNameObjHolder; }
    SceneObjHolder& sceneObjHolder() { return *mSceneObjHolder; }

    // The first half of SceneNameObjMovementController::movement(): apply requested suspends and resumes.
    void movement();
    void requestMovementSync() { mSyncRequested = true; }

private:
    NameObjHolder* mNameObjHolder;
    SceneObjHolder* mSceneObjHolder;
    bool mSyncRequested = false;
};
} // namespace smg
