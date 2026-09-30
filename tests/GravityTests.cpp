#include <Game/Gravity/DiskGravity.hpp>
#include <Game/Gravity/ParallelGravity.hpp>
#include <Game/Gravity/PointGravity.hpp>
#include <Game/Gravity/SegmentGravity.hpp>
#include <Game/Gravity/WireGravity.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance * std::fmax(1.0f, std::fabs(b)); }
bool vec(const TVec3f& v, float x, float y, float z) { return near(v.x, x) && near(v.y, y) && near(v.z, z); }

void pointGravity() {
    PointGravity point;
    point.mOrigPosition.set(0.0f, 0.0f, 0.0f);
    point.updateIdentityMtx();
    TVec3f pull;
    // The original strength falls off with the square of the distance: 4,000,000 / r^2.
    check(point.calcGravity(&pull, TVec3f(0.0f, 10.0f, 0.0f)), "original point gravity attracts");
    check(vec(pull, 0, -40000, 0), "toward the point with inverse-square strength");
    check(point.calcGravity(&pull, TVec3f(0.0f, 0.0f, 2000.0f)) && vec(pull, 0, 0, -1), "unit strength at 2000 units");
    check(point.calcGravity(&pull, TVec3f(0.5f, 0.0f, 0.0f)) && vec(pull, -4000000, 0, 0), "radius clamped to one near the center");

    point.mDistant = 1000.0f;
    check(point.calcGravity(&pull, TVec3f(0.0f, 3000.0f, 0.0f)) && vec(pull, 0, -1, 0), "falloff measured from the planet surface");
    point.mRange = 500.0f;
    check(point.calcGravity(&pull, TVec3f(0.0f, 1400.0f, 0.0f)), "within range of the surface");
    check(!point.calcGravity(&pull, TVec3f(0.0f, 1600.0f, 0.0f)), "no pull past the range");
    point.mRange = -1.0f;
    point.mIsInverse = true;
    check(point.calcGravity(&pull, TVec3f(0.0f, 3000.0f, 0.0f)) && vec(pull, 0, 1, 0), "inverse gravity pushes away");

    TPos3f moved;
    moved.identity();
    moved.mMtx[0][3] = 100.0f;
    point.mIsInverse = false;
    point.mDistant = 0.0f;
    point.updateMtx(moved);
    check(point.calcGravity(&pull, TVec3f(100.0f, 0.0f, 2000.0f)) && vec(pull, 0, 0, -1), "zone matrix moves the center");
}
// Parallel gravity reports its base distance, so the default 2000 gives exactly unit strength.
void parallelGravity() {
    ParallelGravity plane;
    plane.setPlane(TVec3f(0.0f, 1.0f, 0.0f), TVec3f(0.0f, 0.0f, 0.0f));
    plane.updateIdentityMtx();
    TVec3f pull;
    check(plane.calcGravity(&pull, TVec3f(123.0f, 456.0f, -789.0f)) && vec(pull, 0, -1, 0), "uniform unit pull along the plane");
    plane.mRange = 1000.0f;
    check(plane.calcGravity(&pull, TVec3f(0.0f, 900.0f, 0.0f)), "inside the range sphere");
    check(!plane.calcGravity(&pull, TVec3f(0.0f, 1100.0f, 0.0f)), "outside the range sphere");
    plane.mRange = -1.0f;
    plane.setBaseDistance(1000.0f);
    check(plane.calcGravity(&pull, TVec3f(0.0f, 5.0f, 0.0f)) && vec(pull, 0, -4, 0), "a shorter base distance pulls harder");

    TPos3f quarterTurn;
    quarterTurn.identity();
    quarterTurn.mMtx[0][0] = 0.0f; quarterTurn.mMtx[0][1] = -1.0f;
    quarterTurn.mMtx[1][0] = 1.0f; quarterTurn.mMtx[1][1] = 0.0f;
    plane.setBaseDistance(2000.0f);
    plane.updateMtx(quarterTurn);
    check(plane.calcGravity(&pull, TVec3f(0.0f, 0.0f, 0.0f)) && vec(pull, 1, 0, 0), "zone rotation turns the plane");
}
// Over the disk the pull is straight at its plane; past the rim it is toward the nearest rim point.
void diskGravity() {
    DiskGravity disk;
    disk.setLocalPosition(TVec3f(0.0f, 0.0f, 0.0f));
    disk.setRadius(2500.0f);
    disk.updateIdentityMtx();
    TVec3f pull;
    check(disk.calcGravity(&pull, TVec3f(100.0f, 1000.0f, 0.0f)) && vec(pull, 0, -4, 0), "above the disk, falling onto it");
    check(disk.calcGravity(&pull, TVec3f(0.0f, -1000.0f, 0.0f)) && vec(pull, 0, 4, 0), "underside pulls up by default");
    check(disk.calcGravity(&pull, TVec3f(4500.0f, 0.0f, 0.0f)) && vec(pull, -1, 0, 0), "past the rim, toward the rim");
    disk.setBothSide(false);
    check(!disk.calcGravity(&pull, TVec3f(0.0f, -1000.0f, 0.0f)), "one-sided disk ignores its underside");
    disk.setEnableEdgeGravity(false);
    check(!disk.calcGravity(&pull, TVec3f(4500.0f, 0.0f, 0.0f)), "no rim pull when edge gravity is off");
}
// Along the segment the pull is toward the nearest point on it; past an end, toward that end.
void segmentGravity() {
    SegmentGravity segment;
    segment.setGravityPoint(0, TVec3f(0.0f, 0.0f, 0.0f));
    segment.setGravityPoint(1, TVec3f(1000.0f, 0.0f, 0.0f));
    segment.updateIdentityMtx();
    TVec3f pull;
    check(segment.calcGravity(&pull, TVec3f(500.0f, 2000.0f, 0.0f)) && vec(pull, 0, -1, 0), "beside the segment, toward it");
    check(segment.calcGravity(&pull, TVec3f(-2000.0f, 0.0f, 0.0f)) && vec(pull, 1, 0, 0), "before the start, toward the start");
    check(segment.calcGravity(&pull, TVec3f(3000.0f, 0.0f, 0.0f)) && vec(pull, -1, 0, 0), "past the end, toward the end");
    segment.setEdgeValid(1, false);
    check(!segment.calcGravity(&pull, TVec3f(3000.0f, 0.0f, 0.0f)), "a disabled end has no pull");
}
// Wire gravity pulls toward the nearest point of a polyline; its creator samples a rail, which waits for RailRider.
void wireGravity() {
    WireGravity wire;
    wire.setPointListSize(3);
    wire.addPoint(TVec3f(0.0f, 0.0f, 0.0f));
    wire.addPoint(TVec3f(1000.0f, 0.0f, 0.0f));
    wire.addPoint(TVec3f(1000.0f, 1000.0f, 0.0f));
    wire.updateIdentityMtx();
    TVec3f pull;
    check(wire.calcGravity(&pull, TVec3f(500.0f, -2000.0f, 0.0f)) && vec(pull, 0, 1, 0), "toward the first span");
    check(wire.calcGravity(&pull, TVec3f(3000.0f, 500.0f, 0.0f)) && vec(pull, -1, 0, 0), "toward the nearer second span");
    check(wire.calcGravity(&pull, TVec3f(1000.0f, 3000.0f, 0.0f)) && vec(pull, 0, -1, 0), "past the end, toward the end point");
    wire.mRange = 1000.0f;
    check(!wire.calcGravity(&pull, TVec3f(500.0f, -2000.0f, 0.0f)), "no pull past the range");
}
} // namespace

int runGravityTests() {
    pointGravity();
    parallelGravity();
    diskGravity();
    segmentGravity();
    wireGravity();
    std::printf("%u original gravity checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGravityTests(); }
#endif
