// The Wii's OS services that need no hardware: cache controls (nothing outside the CPU reads what they flush;
// the renderer flushes its own buffers for the 3DS GPU), interrupt and scheduler state, the console and panics.
#include <revolution/base/PPCArch.h>
#include <revolution/os.h>
#include <revolution/os/OSCache.h>
#include <revolution/os/OSInterrupt.h>
#include <revolution/os/OSThread.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The Wii's bus clock, which the SDK reads from low memory; OSGetTime's ticks run at a quarter of it.
u32 __OSBusClock = 243000000;

void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCStoreRangeNoSync(void*, u32) {}
void DCInvalidateRange(void*, u32) {}
// dcbz zeroes whole cache lines, the one cache operation whose effect code sees. Only the bytes asked for are
// cleared: the port's heap does not keep the Wii's 32-byte alignment, so rounding out could reach a neighbour.
void DCZeroRange(void* start, u32 size) { std::memset(start, 0, size); }
void PPCSync() { __sync_synchronize(); }

namespace {
BOOL sInterruptsEnabled = TRUE;
s32 sSchedulerDisabled = 0;
} // namespace

// The previous state comes back, as the SDK returns it, so nested sections restore what they found.
BOOL OSDisableInterrupts() {
    const BOOL previous = sInterruptsEnabled;
    sInterruptsEnabled = FALSE;
    return previous;
}

BOOL OSEnableInterrupts() {
    const BOOL previous = sInterruptsEnabled;
    sInterruptsEnabled = TRUE;
    return previous;
}

BOOL OSRestoreInterrupts(BOOL level) {
    const BOOL previous = sInterruptsEnabled;
    sInterruptsEnabled = level;
    return previous;
}

s32 OSDisableScheduler() { return sSchedulerDisabled++; }
s32 OSEnableScheduler() { return sSchedulerDisabled--; }

// The SDK's console output goes to the port's stderr: the terminal on a host, the emulator's log on a 3DS.
void OSVReport(const char* format, va_list arguments) { std::vfprintf(stderr, format, arguments); }

void OSReport(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    OSVReport(format, arguments);
    va_end(arguments);
}

void OSPanic(const char* file, int line, const char* format, ...) {
    std::fprintf(stderr, "%s:%d Panic: ", file, line);
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fputc('\n', stderr);
    std::abort();
}

void PPCHalt() { std::abort(); }
