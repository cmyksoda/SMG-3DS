#include <smg/Scene.hpp>
#include <Game/NameObj/NameObjCategoryList.hpp>
#include <Game/NameObj/NameObjGroup.hpp>
#include <Game/NameObj/NameObjHolder.hpp>
#include <Game/Util/HashUtil.hpp>
#include <Game/Util/ObjUtil.hpp>
#include <Game/Util/SystemUtil.hpp>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

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
    void draw() const override { ++draws; }
    int moves = 0;
    mutable int draws = 0;
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
struct BeforeCategory {
    void run() { ++calls; }
    int calls = 0;
};

// The scheduler's lists, as NameObjListExecutor builds them for movement, calcAnim and draw.
void categoryLists() {
    Counter a("A"), b("B"), c("C");
    const CategoryListInitialTable sizes[]{{0, 4}, {2, 2}, {static_cast<u32>(-1), 0}};
    NameObjCategoryList movement(3, sizes, &NameObj::movement, false, "movement");
    movement.add(&a, 0);
    movement.add(&b, 0);
    movement.add(&c, 2);
    movement.execute(0);
    check(a.moves == 1 && b.moves == 1 && c.moves == 0, "a category runs only its own objects");

    BeforeCategory before;
    movement.registerExecuteBeforeFunction(MR::Functor(&before, &BeforeCategory::run), 1);
    movement.execute(1);
    check(before.calls == 0, "an empty category skips its before-function");
    movement.registerExecuteBeforeFunction(MR::Functor(&before, &BeforeCategory::run), 2);
    movement.execute(2);
    check(c.moves == 1 && before.calls == 1, "the before-function runs ahead of its category");

    movement.remove(&a, 0);
    movement.execute(0);
    check(a.moves == 1 && b.moves == 2, "a removed object stops running");
    // The scene heap frees the cloned functors in the game; FunctorBase has no virtual destructor.
    using Clone = decltype(MR::Functor(&before, &BeforeCategory::run));
    for (int category : {1, 2}) {
        auto* clone = static_cast<Clone*>(movement.mCategoryInfo[category].mExecuteBeforeFunction);
        clone->~Clone();
        ::operator delete(clone);
    }

    // Draw lists count their members first, then allocate exactly that much.
    const CategoryListInitialTable counted[]{{0, 0}, {static_cast<u32>(-1), 0}};
    NameObjCategoryList draw(1, counted, &NameObj::draw, true, "draw");
    draw.incrementCheck(&a, 0);
    draw.incrementCheck(&b, 0);
    draw.allocateBuffer();
    check(draw.mBufferAllocated && draw.mCategoryInfo[0].mNameObjArr.capacity() == 2, "sized by the counted members");
    draw.add(&a, 0);
    draw.add(&b, 0);
    draw.execute(0);
    check(a.draws == 1 && b.draws == 1 && c.draws == 0, "a const list calls draw");
}
} // namespace

int runNameObjTests() {
    hashes();
    {
        smg::NativeScene scene;
        registration(scene);
        movement(scene);
        categoryLists();
        scene.nameObjHolder().clearArray();
    }
    std::printf("%u original NameObj checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runNameObjTests(); }
#endif
