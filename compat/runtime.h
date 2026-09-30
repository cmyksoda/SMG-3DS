#pragma once
// Metrowerks' runtime helpers, which its compiler called for these casts and the game sometimes calls by name;
// here they are the casts themselves.
#ifdef __cplusplus
extern "C" {
#endif
static inline unsigned long __cvt_fp2unsigned(double value) { return (unsigned long)value; }
static inline unsigned long long __cvt_dbl_usll(double value) { return (unsigned long long)value; }
#ifdef __cplusplus
}
#endif
