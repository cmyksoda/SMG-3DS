#pragma once
// MSL's wide-string functions. The Wii's wchar_t is two bytes and these toolchains' four: text that
// reads UTF-16 through wchar_t needs its own handling before it can run (docs/PORTING.md).
#include <wchar.h>
