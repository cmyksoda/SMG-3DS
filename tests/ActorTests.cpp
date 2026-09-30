#include "KclFixture.hpp"
#include <smg/Actors.hpp>
#include <smg/Bcsv.hpp>
#include <smg/Kcl.hpp>
#include <smg/Scene.hpp>
#include <Game/Gravity/ParallelGravity.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/LiveActor/Binder.hpp>
#include <Game/Map/CollisionDirector.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <Game/Util/LiveActorUtil.hpp>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

// An original LiveActor, moved by LiveActor::movement under the original gravity, lands through its Binder
// on a floor that is an original CollisionParts in the original CollisionDirector.
void probeLands() {
    const auto raw = floorKcl(true), table = attributes();
    smg::Kcl kcl;
    smg::Bcsv pa;
    std::string error;
    check(kcl.load({raw.data(), raw.size()}, error) && pa.load({table.data(), table.size()}, error), "the floor's files load");

    ParallelGravity down; // registered gravity outlives the scene that reads it
    down.setPlane(TVec3f(0.0f, 1.0f, 0.0f), TVec3f(0.0f, 0.0f, 0.0f));
    down.updateIdentityMtx();
    smg::NativeScene scene;
    MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager)->registerGravity(&down);
    TPos3f placement;
    placement.identity();
    scene.addZone("TestGalaxy", placement); // a stage's zone 0, where its collision belongs
    new smg::CollisionHost("floor", placement, kcl.data(), pa.info().mData);
    auto* probe = new smg::ProbeActor(TVec3f(50.0f, 200.0f, 50.0f));
    scene.endActorPlacement();
    check(!MR::isDead(probe) && scene.isExecuted(probe), "the probe appears and joins the scene's movement");

    int frames = 0;
    while (!probe->landed() && frames < 600) {
        scene.movement();
        ++frames;
    }
    check(probe->landed(), "the probe lands");
    check(frames > 10 && frames < 60, "after a fall of about twenty frames");
    // Pressed down a unit each frame, the Binder sets the sphere back about a unit above touching, so contact
    // comes every other frame; the sphere never moves more than that.
    const auto resting = [&] { return probe->mPosition.y > 19.9f && probe->mPosition.y < 21.5f && std::fabs(probe->mPosition.x - 50.0f) < 0.01f; };
    check(resting(), "its sphere rests on the floor, where it fell");
    check(probe->mGravity.y < -0.99f, "under the gravity the original manager picked");
    for (int i = 0; i < 2 && !MR::isBindedGround(probe); ++i) scene.movement();
    check(MR::isBindedGround(probe) && probe->getBinder()->mGroundInfo.mParentTriangle.getNormal(0)->y > 0.99f,
          "the Binder's ground is the floor's up-facing triangle");
    bool stays = true;
    for (int i = 0; i < 30; ++i) {
        scene.movement();
        stays &= resting();
    }
    check(stays, "and it stays there");

    // Off the square's edge, nothing holds it.
    auto* past = new smg::ProbeActor(TVec3f(300.0f, 200.0f, 50.0f));
    for (int i = 0; i < 60; ++i) scene.movement();
    check(!past->landed() && past->mPosition.y < 0.0f, "a probe beside the floor falls past it");
}
} // namespace

int runActorTests() {
    probeLands();
    std::printf("%u original actor checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runActorTests(); }
#endif
