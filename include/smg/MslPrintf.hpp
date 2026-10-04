#pragma once
#include <cstdarg>
#include <cstddef>

namespace smg {
// vsnprintf as Metrowerks' MSL behaves where newlib differs: a null %s is an empty string (newlib writes "(null)").
// Each conversion goes to `print`, the C library's snprintf, so everything else is its own.
using SnprintfFunction = int (*)(char*, std::size_t, const char*, ...);
int mslVsnprintf(char* out, std::size_t size, const char* format, va_list arguments, SnprintfFunction print);
} // namespace smg
