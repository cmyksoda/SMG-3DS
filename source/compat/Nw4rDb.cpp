// nw4r's panic: the message goes where OSReport's does and the program stops. The Wii's version also walked the
// PowerPC stack and took over the video to show it, which has no counterpart here.
#include <revolution/os.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace nw4r {
namespace db {
void VPanic(const char* file, int line, const char* format, va_list arguments, bool halt) {
    OSReport("%s:%d Panic:", file, line);
    OSVReport(format, arguments);
    OSReport("\n");
    if (halt) std::abort();
}
} // namespace db
} // namespace nw4r
