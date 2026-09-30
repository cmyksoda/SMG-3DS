#pragma once

// Fixed width types replace Metrowerks' 32-bit long, including in host tests.
#include <stddef.h> // NULL and size_t, which Metrowerks' headers had everywhere
#include <stdint.h>
#ifdef __3DS__
#include <3ds/types.h>
#else
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif
typedef float f32;
typedef double f64;
typedef int BOOL;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;
typedef volatile f32 vf32;
typedef volatile f64 vf64;
#define ATTRIBUTE_ALIGN(n) __attribute__((aligned(n)))
#define ATTRIBUTE_PACKED __attribute__((packed))
#define ATTRIBUTE_WEAK __attribute__((weak))
#define ALWAYS_INLINE __attribute__((always_inline))
// Empty for GCC, as upstream's own types.h has it: Metrowerks takes it after a definition's declarator,
// where GCC refuses attributes, and it only steered code generation for matching.
#define NO_INLINE
#define AT_ADDRESS(x)
#define __REGISTER
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

// Upstream's utility macros (types.h's and macros.h's angles); pointers go through uintptr_t, which is 64
// bits on hosts.
#define ROUND_UP(x, align) (((x) + (align) - 1) & (-(align)))
#define ROUND_UP_PTR(x, align) ((void*)((((uintptr_t)(x)) + (align) - 1) & (~((align) - 1))))
#define ALIGN_PREV(X, N) ((X) & ~((N) - 1))
#define ALIGN_NEXT(X, N) ALIGN_PREV(((X) + (N) - 1), N)
#define ARRAY_SIZE(o) (s32)(sizeof(o) / sizeof(o[0]))
#define ARRAY_SIZEU(o) (sizeof(o) / sizeof(o[0]))
#ifndef MAX
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#endif
#ifndef MIN
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#endif
#define FOURCC(c0, c1, c2, c3) (u32)((c0 & 0xFF) << 24 | (c1 & 0xFF) << 16 | (c2 & 0xFF) << 8 | (c3 & 0xFF))
#define IS_ALIGNED(x, align) (((uintptr_t)(x) & ((align) - 1)) == 0)
#define IS_NOT_ALIGNED(X, N) (((X) & ((N) - 1)) != 0)
#ifndef M_PI_F
#define M_PI_F 3.1415926f
#endif
#define DEG_TO_RAD_MULT_CONSTANT (M_PI_F / 180.0f)
#define RAD_TO_DEG_MULT_CONSTANT (180.0f / M_PI_F)
#define DEG_TO_RAD(x) ((x) * DEG_TO_RAD_MULT_CONSTANT)
#define RAD_TO_DEG(x) ((x) * RAD_TO_DEG_MULT_CONSTANT)
#define IS_REF_NULL(r) (0)
#define IS_REF_NONNULL(r) (1)

// Metrowerks intrinsics, declared as upstream declares them for other compilers; source/compat/Math.cpp has
// them. __fabs and __fabsf are left to the C library, which declares its own.
f32 __frsqrte(f32);
u32 __cntlzw(u32);
s32 __abs(s32);
void* __memcpy(void*, const void*, int);
