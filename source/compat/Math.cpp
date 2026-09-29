#include <JSystem/JMath/JMath.hpp>
#include <JSystem/JMath/JMATrigonometric.hpp>
#include <cmath>

const Vec gZeroVec{0, 0, 0};

// Scalar replacements for Gekko paired-single instructions. Outputs may alias inputs.
namespace JMathInlineVEC {
void PSVECCopy(const Vec* a, Vec* out) { *out = *a; }
void PSVECAdd(const Vec* a, const Vec* b, Vec* out) { *out = {a->x+b->x, a->y+b->y, a->z+b->z}; }
void PSVECSubtract(const Vec* a, const Vec* b, Vec* out) { *out = {a->x-b->x, a->y-b->y, a->z-b->z}; }
void PSVECMultiply(const Vec* a, const Vec* b, Vec* out) { *out = {a->x*b->x, a->y*b->y, a->z*b->z}; }
f32 PSVECDotProduct(const Vec* a, const Vec* b) { return a->x*b->x + a->y*b->y + a->z*b->z; }
f32 PSVECSquareMag(const Vec* a) { return JMathInlineVEC::PSVECDotProduct(a, a); }
void PSVECNegate(const Vec* a, Vec* out) { *out = {-a->x, -a->y, -a->z}; }
f32 PSVECSquareDistance(const Vec* a, const Vec* b) {
    const Vec delta{a->x-b->x, a->y-b->y, a->z-b->z};
    return PSVECSquareMag(&delta);
}
}

extern "C" {
f32 PSQUATDotProduct(const Quaternion* a, const Quaternion* b) {
    return a->x*b->x + a->y*b->y + a->z*b->z + a->w*b->w;
}
void PSVECAdd(const Vec* a, const Vec* b, Vec* out) { JMathInlineVEC::PSVECAdd(a,b,out); }
void PSVECSubtract(const Vec* a, const Vec* b, Vec* out) { JMathInlineVEC::PSVECSubtract(a,b,out); }
void PSVECScale(const Vec* a, Vec* out, f32 scale) { *out = {a->x*scale, a->y*scale, a->z*scale}; }
f32 PSVECDotProduct(const Vec* a, const Vec* b) { return JMathInlineVEC::PSVECDotProduct(a,b); }
f32 PSVECMag(const Vec* a) { return std::sqrt(JMathInlineVEC::PSVECSquareMag(a)); }
f32 C_VECMag(const Vec* a) { return PSVECMag(a); }
f32 PSVECDistance(const Vec* a, const Vec* b) { return std::sqrt(JMathInlineVEC::PSVECSquareDistance(a,b)); }
void PSVECNormalize(const Vec* a, Vec* out) { PSVECScale(a, out, 1.0f/PSVECMag(a)); }
void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* out) {
    *out = {a->y*b->z-a->z*b->y, a->z*b->x-a->x*b->z, a->x*b->y-a->y*b->x};
}
}

void JMAVECLerp(const Vec* a, const Vec* b, Vec* out, f32 t) {
    *out = {a->x+t*(b->x-a->x), a->y+t*(b->y-a->y), a->z+t*(b->z-a->z)};
}
void JMAVECScaleAdd(const Vec* a, const Vec* b, Vec* out, f32 scale) {
    *out = {a->x*scale+b->x, a->y*scale+b->y, a->z*scale+b->z};
}
void JMAQuatLerp(const Quaternion* a, const Quaternion* b, f32 t, Quaternion* out) {
    const f32 sign = a->x*b->x+a->y*b->y+a->z*b->z+a->w*b->w < 0 ? -1.0f : 1.0f;
    *out = {a->x+t*(sign*b->x-a->x), a->y+t*(sign*b->y-a->y),
            a->z+t*(sign*b->z-a->z), a->w+t*(sign*b->w-a->w)};
}
void JMAEulerToQuat(s16 x, s16 y, s16 z, Quaternion* out) {
    const f32 cx = JMASCos(x/2), cy = JMASCos(y/2), cz = JMASCos(z/2);
    const f32 sx = JMASSin(x/2), sy = JMASSin(y/2), sz = JMASSin(z/2);
    *out = {sx*cy*cz-cx*sy*sz, cx*sy*cz+sx*cy*sz, cx*cy*sz-sx*sy*cz, cx*cy*cz+sx*sy*sz};
}
void JMAMTXApplyScale(const Mtx src, Mtx dst, f32 x, f32 y, f32 z) {
    for (int row=0; row<3; ++row) {
        dst[row][0] = src[row][0]*x;
        dst[row][1] = src[row][1]*y;
        dst[row][2] = src[row][2]*z;
        dst[row][3] = src[row][3];
    }
}
