#pragma once

// Fixed width types replace Metrowerks' 32-bit long, including in host tests.
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
#define NO_INLINE __attribute__((noinline))
#define AT_ADDRESS(x)
#define __REGISTER
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
