#include <smg/Scene.hpp>
#include <Game/Gravity/GravityInfo.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/Gravity/PointGravity.hpp>
#include <Game/NameObj/NameObjHolder.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }
bool vec(const TVec3f& v, float x, float y, float z) { return near(v.x, x) && near(v.y, y) && near(v.z, z); }

void place(PointGravity& gravity, float x, float y, float z, s32 priority) {
    gravity.setOrig(TVec3f(x, y, z));
    gravity.setPriority(priority);
    gravity.updateIdentityMtx();
}

void gravityManager(PlanetGravityManager& manager) {
    TVec3f pull(9.0f, 9.0f, 9.0f);
    GravityInfo info;
    check(!manager.calcTotalGravityVector(&pull, &info, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0), "no gravity, no pull");
    check(vec(pull, 0, 0, 0) && info.mGravityInstance == nullptr, "an empty result is zero");

    PointGravity below, beside, near;
    place(below, 0, -2000, 0, 0);
    place(beside, 2000, 0, 0, 0);
    manager.registerGravity(&below);
    manager.registerGravity(&beside);
    check(below.mIsRegistered && beside.mIsRegistered, "registering marks each gravity");
    check(manager.calcTotalGravityVector(&pull, &info, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0), "two planets pull");
    const float half = std::sqrt(0.5f);
    check(vec(pull, half, -half, 0), "equal priorities add, then normalize");
    check(info.mGravityInstance == &below && info.mLargestPriority == 0, "ties keep the first registered as the info");

    // Registered after the others, but its priority sorts it first.
    place(near, 0, 0, 1000, 10);
    near.mRange = 2000.0f;
    manager.registerGravity(&near);
    check(manager.calcTotalGravityVector(&pull, &info, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0), "the nearby planet pulls");
    check(vec(pull, 0, 0, 1) && info.mGravityInstance == &near && info.mLargestPriority == 10,
          "a higher priority replaces lower ones");
    check(manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, -5000), GRAVITY_TYPE_NORMAL, 0), "out of its range");
    check(!near.calcGravity(&pull, TVec3f(0, 0, -5000)), "the nearby planet's range ends");

    check(manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL,
                                         static_cast<u32>(reinterpret_cast<std::uintptr_t>(near.mHost))),
          "a requester still feels other gravity");
    check(vec(pull, half, -half, 0), "a requester is not pulled by gravity it hosts");

    near.mGravityType = GRAVITY_TYPE_SHADOW;
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0);
    check(vec(pull, half, -half, 0), "a shadow-only gravity leaves normal gravity alone");
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL | GRAVITY_TYPE_SHADOW, 0);
    check(vec(pull, 0, 0, 1), "and counts when shadows ask");
    near.mGravityType = GRAVITY_TYPE_NORMAL;

    near.mActivated = false;
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0);
    check(vec(pull, half, -half, 0), "a switched-off gravity is skipped");
    near.mActivated = true;
    beside.mAppeared = false;
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, -5000), GRAVITY_TYPE_NORMAL, 0);
    const float d = std::sqrt(2000.0f * 2000.0f + 5000.0f * 5000.0f);
    check(vec(pull, 0, -2000.0f / d, 5000.0f / d), "a dead gravity is skipped");
}

void sceneObjects() {
    check(!MR::isExistSceneObj(SceneObj_PlanetGravityManager), "no scene objects before a scene");
    smg::NativeScene scene;
    check(MR::isExistSceneObj(SceneObj_PlanetGravityManager), "the scene makes its gravity manager, as GameScene does");
    check(!MR::isExistSceneObj(SceneObj_StageDataHolder), "objects not brought up are absent");
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    check(MR::createSceneObj(SceneObj_PlanetGravityManager) == manager, "creating it again returns the same object");
    check(scene.nameObjHolder().find("重力") == manager, "it registered as a NameObj under its original name");
    gravityManager(*manager);
}
} // namespace

int runSceneTests() {
    sceneObjects();
    check(!MR::isExistSceneObj(SceneObj_PlanetGravityManager), "the scene's objects go with it");
    std::printf("%u original scene and gravity checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runSceneTests(); }
#endif
