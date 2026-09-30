#pragma once
// MSL's extras.h: stricmp, its name for the C library's ASCII case-blind compare.
#include <strings.h>
static inline int stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
