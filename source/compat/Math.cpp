#include <JSystem/JMath/JMath.hpp>
#include <JSystem/JMath/JMATrigonometric.hpp>
#include <revolution/mtx.h>
#include <cmath>
#include <cstring>

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
void PSMTXCopy(const Mtx src, Mtx dst) {
    if (src == dst) return;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col) dst[row][col] = src[row][col];
}
void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    const Vec v = *src;
    *dst = {m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z + m[0][3],
            m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z + m[1][3],
            m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z + m[2][3]};
}
void PSMTXRotAxisRad(Mtx m, const Vec* axis, f32 radians) {
    Vec a;
    PSVECNormalize(axis, &a);
    const f32 s = std::sin(radians), c = std::cos(radians), t = 1.0f - c;
    const f32 rows[3][4]{{t*a.x*a.x + c, t*a.x*a.y - s*a.z, t*a.x*a.z + s*a.y, 0},
                         {t*a.x*a.y + s*a.z, t*a.y*a.y + c, t*a.y*a.z - s*a.x, 0},
                         {t*a.x*a.z - s*a.y, t*a.y*a.z + s*a.x, t*a.z*a.z + c, 0}};
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col) m[row][col] = rows[row][col];
}
void PSMTXConcat(const Mtx a, const Mtx b, Mtx out) {
    f32 r[3][4];
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            r[row][col] = a[row][0]*b[0][col] + a[row][1]*b[1][col] + a[row][2]*b[2][col] + (col == 3 ? a[row][3] : 0);
        }
    }
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col) out[row][col] = r[row][col];
}
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    const Vec v = *src;
    *dst = {m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z,
            m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z,
            m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z};
}
u32 PSMTXInverse(const Mtx m, Mtx out) {
    const f32 c00 = m[1][1]*m[2][2] - m[1][2]*m[2][1], c01 = m[1][2]*m[2][0] - m[1][0]*m[2][2];
    const f32 c02 = m[1][0]*m[2][1] - m[1][1]*m[2][0];
    const f32 det = m[0][0]*c00 + m[0][1]*c01 + m[0][2]*c02;
    if (det == 0.0f) return 0;
    const f32 k = 1.0f / det;
    f32 r[3][4];
    r[0][0] = c00*k;
    r[0][1] = (m[0][2]*m[2][1] - m[0][1]*m[2][2])*k;
    r[0][2] = (m[0][1]*m[1][2] - m[0][2]*m[1][1])*k;
    r[1][0] = c01*k;
    r[1][1] = (m[0][0]*m[2][2] - m[0][2]*m[2][0])*k;
    r[1][2] = (m[0][2]*m[1][0] - m[0][0]*m[1][2])*k;
    r[2][0] = c02*k;
    r[2][1] = (m[0][1]*m[2][0] - m[0][0]*m[2][1])*k;
    r[2][2] = (m[0][0]*m[1][1] - m[0][1]*m[1][0])*k;
    for (int row = 0; row < 3; ++row)
        r[row][3] = -(r[row][0]*m[0][3] + r[row][1]*m[1][3] + r[row][2]*m[2][3]);
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col) out[row][col] = r[row][col];
    return 1;
}
void PSMTXQuat(Mtx m, const Quaternion* q) {
    const f32 n = q->x*q->x + q->y*q->y + q->z*q->z + q->w*q->w;
    const f32 s = n > 0 ? 2.0f / n : 0.0f;
    const f32 x = q->x, y = q->y, z = q->z, w = q->w;
    const f32 r[3][4]{{1 - s*(y*y + z*z), s*(x*y - w*z), s*(x*z + w*y), 0},
                      {s*(x*y + w*z), 1 - s*(x*x + z*z), s*(y*z - w*x), 0},
                      {s*(x*z - w*y), s*(y*z + w*x), 1 - s*(x*x + y*y), 0}};
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col) m[row][col] = r[row][col];
}
void C_QUATMtx(Quaternion* out, const Mtx m) {
    // Take the square root of the largest diagonal term to keep the division well conditioned.
    const f32 trace = m[0][0] + m[1][1] + m[2][2];
    Quaternion q;
    if (trace > 0) {
        const f32 s = std::sqrt(trace + 1.0f) * 2;
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, s / 4};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const f32 s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = {s / 4, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const f32 s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = {(m[0][1] + m[1][0]) / s, s / 4, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const f32 s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, s / 4, (m[1][0] - m[0][1]) / s};
    }
    *out = q;
}
void C_QUATSlerp(const Quaternion* p, const Quaternion* q, Quaternion* out, f32 t) {
    Quaternion b = *q;
    f32 cosine = PSQUATDotProduct(p, &b);
    if (cosine < 0) { // the shorter way round
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }
    f32 wa = 1 - t, wb = t;
    if (cosine < 0.9999f) {
        const f32 angle = std::acos(cosine), s = std::sin(angle);
        wa = std::sin((1 - t) * angle) / s;
        wb = std::sin(t * angle) / s;
    }
    *out = {wa*p->x + wb*b.x, wa*p->y + wb*b.y, wa*p->z + wb*b.z, wa*p->w + wb*b.w};
}
void PSQUATMultiply(const Quaternion* p, const Quaternion* q, Quaternion* out) {
    const Quaternion a = *p, b = *q;
    *out = {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y + a.y*b.w + a.z*b.x - a.x*b.z,
            a.w*b.z + a.z*b.w + a.x*b.y - a.y*b.x,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
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

// The PowerPC intrinsics. frsqrte is an estimate good to 1/32; callers refine it, and the exact root
// only makes their result closer.
f32 __frsqrte(f32 x) { return 1.0f / std::sqrt(x); }
u32 __cntlzw(u32 x) { return x ? __builtin_clz(x) : 32; }
s32 __abs(s32 x) { return x < 0 ? -x : x; }
void* __memcpy(void* dst, const void* src, int n) { return std::memcpy(dst, src, n); }
