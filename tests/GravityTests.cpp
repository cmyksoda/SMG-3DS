#include <Game/Gravity/ParallelGravity.hpp>
#include <Game/Gravity/PointGravity.hpp>
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
} // namespace

int runGravityTests() {
    pointGravity();
    parallelGravity();
    std::printf("%u original gravity checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGravityTests(); }
#endif
