#include <smg/MslPrintf.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
void check(bool passed, const char* reason) {
    ++checks;
    if (!passed) std::fprintf(stderr, "FAIL: %s\n", reason), std::exit(1);
}

int msl(char* out, std::size_t size, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int made = smg::mslVsnprintf(out, size, format, arguments, std::snprintf);
    va_end(arguments);
    return made;
}

// Everything but a null %s comes out as the C library's own snprintf has it, the length returned included.
#define SAME(...)                                                                                                      \
    do {                                                                                                               \
        char want[256], got[256];                                                                                      \
        const int wantLength = std::snprintf(want, sizeof want, __VA_ARGS__);                                          \
        const int gotLength = msl(got, sizeof got, __VA_ARGS__);                                                       \
        if (wantLength != gotLength || std::strcmp(want, got)) std::fprintf(stderr, "want \"%s\", got \"%s\"\n", want, got); \
        check(wantLength == gotLength && !std::strcmp(want, got), "as snprintf: " #__VA_ARGS__);                     \
    } while (0)
} // namespace

int runMslPrintfTests() {
    SAME("plain text, no conversions");
    SAME("%d|%5d|%-5d|%05d|%+d|% d", -42, 42, 42, 42, 42, 42);
    SAME("%u %x %X %o %#x %#o", 4000000000u, 0xBEEFu, 0xBEEFu, 8u, 255u, 8u);
    SAME("%ld %lu %lld %llu %zu %hhd %hd", -7L, 7UL, -1234567890123LL, 1234567890123ULL, std::size_t(99), 300, 70000);
    SAME("%c%c%c", 'M', 'i', 'o');
    SAME("[%s] [%10s] [%-10s] [%.3s] [%*s] [%.*s]", "star", "star", "star", "starbits", 6, "luma", 2, "luma");
    SAME("%f %.2f %10.3f %e %g %G %.0f", 3.14159, 2.5, -1.0 / 3, 12345.678, 0.0001, 1e20, 0.5);
    SAME("%p and %%", static_cast<void*>(&checks));
    SAME("%s/%s/%03d.arc", "StageData", "AstroGalaxy", 7);

    char small[5];
    const int made = msl(small, sizeof small, "%s", "Rosalina");
    check(made == 8 && !std::strcmp(small, "Rosa"), "a short buffer is cut as snprintf cuts it, the whole length returned");
    check(msl(nullptr, 0, "%d-%s", 123, "abc") == 7, "with no buffer only the length is worked out");

    char out[64];
    msl(out, sizeof out, "[%s]", static_cast<const char*>(nullptr));
    check(!std::strcmp(out, "[]"), "a null %s is empty, as MSL has it");
    msl(out, sizeof out, "[%5s]", static_cast<const char*>(nullptr));
    check(!std::strcmp(out, "[     ]"), "and is padded to its width like any string");
    msl(out, sizeof out, "%s%s.arc", "ObjectData/", static_cast<const char*>(nullptr));
    check(!std::strcmp(out, "ObjectData/.arc"), "an archive path with a null part reads as MSL built it");
    int written = 0;
    msl(out, sizeof out, "abc%n%d", &written, 5);
    check(written == 3 && !std::strcmp(out, "abc5"), "%n counts what came before it");
    std::printf("%u MSL printf checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runMslPrintfTests(); }
#endif
