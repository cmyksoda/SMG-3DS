#include <smg/Scene.hpp>
#include <Game/NameObj/NameObjGroup.hpp>
#include <Game/NameObj/NameObjHolder.hpp>
#include <Game/Util/HashUtil.hpp>
#include <Game/Util/ObjUtil.hpp>
#include <Game/Util/SystemUtil.hpp>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

struct Counter : NameObj {
    explicit Counter(const char* name) : NameObj(name) {}
    void init(const JMapInfoIter& iter) override { initialized = !iter.isValid(); }
    void movement() override { ++moves; }
    int moves = 0;
    bool initialized = false;
};

void hashes() {
    check(MR::getHashCode("Mario") == 74113764u, "the game's string hash");
    check(MR::getHashCode("PlanetGravityManager") == 1993894231u, "a hash that wraps past 32 bits");
    // "重力", the gravity manager's name, in Shift-JIS: bytes above 0x7F count as unsigned, as on the Wii.
    check(MR::getHashCode("\x8f\x64\x97\xcd") == 4361099u, "Shift-JIS bytes hash unsigned");
    check(MR::getHashCodeLower("MaRiO") == MR::getHashCode("mario"), "lowercase hash folds ASCII");
    check(MR::getHashCodeLower("\x8f\x64\x97\xcd") == 4361099u, "lowercase hash leaves Shift-JIS alone");
}

void registration(smg::NativeScene& scene) {
    Counter alpha("Alpha"), beta("Beta");
    NameObjHolder& holder = scene.nameObjHolder();
    check(holder.find("Beta") == &beta, "a new NameObj registers itself with the scene");
    check(holder.find("Beta") == &beta, "a repeated find hits the recent-name cache");
    check(holder.find("Alpha") == &alpha, "every registered object is found");
    check(holder.find("Gamma") == nullptr, "an unknown name finds nothing");
    alpha.setName("Gamma");
    check(holder.find("Gamma") == &alpha, "a renamed object is found by its new name");
    beta.initWithoutIter();
    check(beta.initialized, "initWithoutIter passes an invalid iterator");
    holder.clearArray();
}

void movement(smg::NativeScene& scene) {
    Counter actor("Actor");
    actor.executeMovement();
    check(actor.moves == 1, "movement runs by default");

    NameObjFunction::requestMovementOff(&actor);
    actor.executeMovement();
    check(actor.moves == 2, "a suspend waits for the scene's next sync");
    scene.movement();
    actor.executeMovement();
    check(actor.moves == 2, "suspended after the sync");

    MR::requestMovementOn(&actor);
    scene.movement();
    actor.executeMovement();
    check(actor.moves == 3, "resumed after the next sync");

    actor.requestSuspend();
    scene.movement();
    actor.executeMovement();
    check(actor.moves == 4, "the scene syncs only when a request tells it to");
    actor.requestResume();
    actor.syncWithFlags();
    check(actor.getFlag() == 0, "a resume cancels a pending suspend");

    Counter other("Other");
    MR::suspendAllSceneNameObj();
    MR::syncWithFlagsAllSceneNameObj();
    actor.executeMovement();
    other.executeMovement();
    check(actor.moves == 4 && other.moves == 0, "the scene suspends every object at once");

    NameObjGroup group("IgnorePauseNameObj", 16);
    group.registerObj(&other);
    check(group.getObjNum() == 1 && group.getObj(0) == &other, "a group holds its members");
    group.pauseOffAll();
    scene.movement();
    actor.executeMovement();
    other.executeMovement();
    check(actor.moves == 4 && other.moves == 1, "a group resumes only its members");

    MR::resumeAllSceneNameObj();
    scene.movement();
    actor.executeMovement();
    check(actor.moves == 5, "the scene resumes every object at once");
    scene.nameObjHolder().clearArray();
}
} // namespace

int runNameObjTests() {
    hashes();
    {
        smg::NativeScene scene;
        registration(scene);
        movement(scene);
    }
    std::printf("%u original NameObj checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runNameObjTests(); }
#endif
