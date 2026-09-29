#include <JSystem/JGeometry/TVec.hpp>
#include <JSystem/JMath/JMath.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}
bool near(float a, float b, float tolerance = 0.00001f) { return std::fabs(a-b) <= tolerance; }
bool vec(const Vec& v, float x, float y, float z) { return near(v.x,x) && near(v.y,y) && near(v.z,z); }
}

int runMathTests() {
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
