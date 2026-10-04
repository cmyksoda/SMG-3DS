// The game's sprintf family as MSL behaved (smg::mslVsnprintf): the link wraps newlib's (CMakeLists.txt).
#include <smg/MslPrintf.hpp>
#include <cstdint>

extern "C" {
int __real_snprintf(char* out, std::size_t size, const char* format, ...);

int __wrap_vsnprintf(char* out, std::size_t size, const char* format, va_list arguments) {
    return smg::mslVsnprintf(out, size, format, arguments, __real_snprintf);
}
int __wrap_vsprintf(char* out, const char* format, va_list arguments) {
    return smg::mslVsnprintf(out, SIZE_MAX / 2, format, arguments, __real_snprintf);
}
int __wrap_snprintf(char* out, std::size_t size, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int made = __wrap_vsnprintf(out, size, format, arguments);
    va_end(arguments);
    return made;
}
int __wrap_sprintf(char* out, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int made = __wrap_vsprintf(out, format, arguments);
    va_end(arguments);
    return made;
}
}
