#include <JSystem/JGeometry/TVec.hpp>
#include <JSystem/JMath/JMath.hpp>
#include <Game/Util/MathUtil.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" u32 PSMTXInvXpose(const Mtx, Mtx);  // source/compat/Math.cpp; the SDK's header leaves it out

namespace {
unsigned checks = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}
bool near(float a, float b, float tolerance = 0.00001f) { return std::fabs(a-b) <= tolerance; }
bool vec(const Vec& v, float x, float y, float z) { return near(v.x,x) && near(v.y,y) && near(v.z,z); }
}

void sdkMatrices() {
    Mtx m{{1, 0, 0, 1}, {0, 1, 0, 2}, {0, 0, 1, 3}};
    Vec v{1, 1, 1};
    PSMTXMultVec(m, &v, &v);
    check(vec(v, 2, 3, 4), "matrix times point applies translation, in place");
    const Vec z{0, 0, 5};
    PSMTXRotAxisRad(m, &z, 1.5707963f);
    Vec x{1, 0, 0};
    PSMTXMultVec(m, &x, &x);
    check(vec(x, 0, 1, 0) && m[0][3] == 0 && m[2][3] == 0, "right-handed axis rotation about an unnormalized axis");
    Quaternion i{1, 0, 0, 0}, j{0, 1, 0, 0}, one{0, 0, 0, 1};
    PSQUATMultiply(&i, &j, &i);
    check(near(i.x, 0) && near(i.y, 0) && near(i.z, 1) && near(i.w, 0), "quaternion ij = k, in place");
    PSQUATMultiply(&one, &j, &i);
    check(near(i.y, 1) && near(i.w, 0), "identity quaternion product");

    const f32 h = std::sqrt(0.5f);
    const Quaternion quarterZ{0, 0, h, h};
    Mtx rotation, moved{{1, 0, 0, 5}, {0, 2, 0, 6}, {0, 0, 1, 7}}, both, inverse, identity;
    PSMTXQuat(rotation, &quarterZ);
    Vec p{1, 0, 0};
    PSMTXMultVecSR(rotation, &p, &p);
    check(vec(p, 0, 1, 0), "quaternion to matrix: a quarter turn about Z");
    PSMTXConcat(moved, rotation, both);
    check(PSMTXInverse(both, inverse) == 1, "invertible affine matrix");
    PSMTXConcat(inverse, both, identity);
    bool isIdentity = true;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col)
        isIdentity &= near(identity[row][col], row == col ? 1.0f : 0.0f);
    check(isIdentity, "inverse times matrix is identity");
    const Mtx flat{{1, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 1, 0}};
    check(PSMTXInverse(flat, inverse) == 0, "singular matrix reports failure");
    Mtx normals;
    check(PSMTXInvXpose(moved, normals) == 1 && near(normals[0][0], 1) && near(normals[1][1], 0.5f) && near(normals[2][2], 1) &&
              near(normals[1][3], 0) && near(normals[0][1], 0), "the inverse transpose scales normals back and keeps no translation");
    check(PSMTXInvXpose(flat, normals) == 0, "and refuses a singular matrix");
    Vec shifted{1, 1, 1};
    PSMTXMultVecSR(moved, &shifted, &shifted);
    check(vec(shifted, 1, 2, 1), "rotation-and-scale product ignores translation");
    Quaternion back;
    C_QUATMtx(&back, rotation);
    check(near(std::fabs(PSQUATDotProduct(&back, &quarterZ)), 1), "matrix to quaternion round trip");
    const Quaternion start{0, 0, 0, 1};
    Quaternion half;
    C_QUATSlerp(&start, &quarterZ, &half, 0.5f);
    check(near(half.z, std::sin(3.14159265f / 8)) && near(half.w, std::cos(3.14159265f / 8)), "slerp halfway is an eighth turn");
    const Quaternion flipped{0, 0, -h, -h};
    C_QUATSlerp(&start, &flipped, &half, 0.5f);
    check(near(std::fabs(half.z), std::sin(3.14159265f / 8)), "slerp takes the shorter way round");
}

// Original MR helpers whose only bodies were paired-single assembly.
void pairedSingleHelpers() {
    TVec3f kept;
    const f32 removed = MR::vecKillElement(TVec3f(1, 2, 3), TVec3f(0, 1, 0), &kept);
    check(removed == 2 && kept.x == 1 && kept.y == 0 && kept.z == 3, "vecKillElement removes the killed direction");
    TVec3f sum(1, 1, 1);
    const TVec3f step(1, 2, 3);
    MR::vecScaleAdd(&sum, &step, 2);
    check(sum.x == 3 && sum.y == 5 && sum.z == 7, "vecScaleAdd accumulates in place");
    TVec3f blend;
    MR::vecBlend(TVec3f(0, 0, 0), TVec3f(10, 20, 30), &blend, 0.25f);
    check(near(blend.x, 2.5f) && near(blend.y, 5) && near(blend.z, 7.5f), "vecBlend weights both ends");
}

int runMathTests() {
    sdkMatrices();
    pairedSingleHelpers();
    check(near(JMAFastSqrt(9),3), "JMA square root");
    check(JMAFastSqrt(-4) == -4 && std::signbit(JMAFastSqrt(-0.0f)), "original nonpositive square root convention");
    check(near(JGeometry::TUtil<float>::sqrt(25),5), "geometry square root");
    check(near(JGeometry::TUtil<float>::inv_sqrt(16),0.25f), "inverse square root");
    check(near(TVec2f(3,4).distance(TVec2f(0,0)),5), "stick distance");
    const auto sum = TVec2f(1,2).addInline(TVec2f(3,4));
    check(sum.x == 4 && sum.y == 6, "TVec2 addition has value lifetime");
    const auto sum2 = TVec2f(1,2).addOperatorInline(TVec2f(3,4));
    check(sum2.x == 4 && sum2.y == 6, "TVec2 operator helper has value lifetime");

    using Copy = void(*)(void*,const void*);
    const std::array<Copy,4> copies{JMath::gekko_ps_copy3,JMath::gekko_ps_copy6,JMath::gekko_ps_copy12,JMath::gekko_ps_copy16};
    const std::array<int,4> lengths{3,6,12,16};
    for (unsigned i=0; i<copies.size(); ++i) {
        std::array<float,20> values{}, expected{};
        for (unsigned j=0; j<values.size(); ++j) values[j] = expected[j] = j+0.5f;
        std::memmove(expected.data()+1,expected.data(),lengths[i]*sizeof(float));
        copies[i](values.data()+1,values.data());
        check(values == expected,"paired-single copy overlap and bounds");
        copies[i](values.data(),values.data());
        check(values == expected,"paired-single copy self alias");
    }

    TVec3f a(1.0f,2.0f,3.0f), b(4.0f,-5.0f,6.0f), out(a);
    check(vec(out,1,2,3),"original vector copy constructor");
    out = b; check(vec(out,4,-5,6),"original vector assignment");
    out = a+b; check(vec(out,5,-3,9),"vector sum");
    out = a-b; check(vec(out,-3,7,-3),"vector difference");
    out = a*b; check(vec(out,4,-10,18),"vector component product");
    out *= out; check(vec(out,16,100,324),"component product self alias");
    check(near(a.dot(b),12),"dot product");
    check(near(a.squared(b),67),"squared distance");
    check(near(a.squared(),14),"squared magnitude");
    out = a.cross(b); check(vec(out,27,6,-13),"cross product handedness");
    PSVECCrossProduct(&a,&b,&a); check(vec(a,27,6,-13),"cross product left alias");
    a.set(1,2,3); PSVECCrossProduct(&a,&b,&b); check(vec(b,27,6,-13),"cross product right alias");
    a.set(0,3,4); check(near(a.normalize(),5) && vec(a,0,0.6f,0.8f),"normalization length and in-place result");
    a.negate(); check(vec(a,0,-0.6f,-0.8f),"in-place negate");
    a.setPSZeroVec(); check(vec(a,0,0,0),"paired-single zero");
    a.set(1,2,3); b.set(5,6,7); a.lerp(a,b,0.25f); check(vec(a,2,3,4),"aliased vector interpolation");
    a.scaleAdd(2,a,b); check(vec(a,9,12,15),"aliased scale add");
    check(near(TVec3f(1,0,0).angle(TVec3f(0,1,0)),HALF_PI),"vector angle via original atan table");

    auto curve = [](float t) { return JMAHermiteInterpolation(t,2,3,4,5,-2,-1); };
    check(near(curve(2),3) && near(curve(5),-2),"Hermite endpoints with nonunit time interval");
    check(near(curve(3.5f),2.375f),"Hermite midpoint tangent contribution");
    check(near((curve(2.001f)-curve(1.999f))/0.002f,4,0.002f),"Hermite start derivative");
    check(near((curve(5.001f)-curve(4.999f))/0.002f,-1,0.005f),"Hermite end derivative");

    Quaternion q{0,0,0,1}, opposite{0,0,0,-1};
    JMAQuatLerp(&q,&opposite,0.5f,&q);
    check(near(q.w,1),"quaternion shortest hemisphere interpolation");
    Quaternion quarter{0,0,1,0};
    JMAQuatLerp(&q,&quarter,0.5f,&quarter);
    check(near(quarter.z,0.5f) && near(quarter.w,0.5f),"quaternion interpolation right alias");
    JMAEulerToQuat(0,0,16384,&q);
    check(near(q.z,std::sqrt(0.5f)) && near(q.w,std::sqrt(0.5f)),"Wii short angle to quaternion");
    TQuat4f normalized(0.0f,0.0f,0.0f,4.0f), normalizedCopy;
    normalizedCopy.normalize(normalized);
    check(near(normalizedCopy.w,1),"quaternion reciprocal square root");
    Mtx matrix{{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    JMAMTXApplyScale(matrix,matrix,2,3,4);
    check(matrix[0][0]==2 && matrix[1][1]==18 && matrix[2][2]==44 && matrix[2][3]==12,
          "local matrix scale preserves translation");

    for (int i=-180; i<=180; ++i) {
        const float radians = i*PI/180;
        check(near(JMASinDegree(i),std::sin(radians),0.0004f),"original quantized sine table");
        check(near(JMACosDegree(i),std::cos(radians),0.0004f),"original quantized cosine table");
    }
    for (int y=-8; y<=8; ++y) for (int x=-8; x<=8; ++x)
        check(near(JMAATan2(y,x),std::atan2(static_cast<float>(y),static_cast<float>(x)),0.0006f),
              "atan table quadrants, diagonal endpoint and origin");
    check(near(JMAAsinRadian(1),HALF_PI) && near(JMAAcosRadian(-1),PI),"inverse trig endpoint clamp");
    std::printf("%u portable math checks passed\n",checks);
    return 0;
}
#ifndef SMG_TEST_RUNNER
int main() { return runMathTests(); }
#endif
