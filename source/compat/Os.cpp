// The Wii's cache, interrupt and scheduler controls where J3D and the SDK use them. The port has no reader
// outside the CPU for what they flush (the renderer hands its own buffers to the 3DS GPU with
// GSPGPU_FlushDataCache), and J3D runs on one thread, so each does what its purpose needs here: nothing.
#include <revolution/base/PPCArch.h>
#include <revolution/os/OSCache.h>
#include <revolution/os/OSInterrupt.h>
#include <revolution/os/OSThread.h>

void DCFlushRange(void*, u32) {}
void DCStoreRange(void*, u32) {}
void DCStoreRangeNoSync(void*, u32) {}
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

BOOL OSRestoreInterrupts(BOOL level) {
    const BOOL previous = sInterruptsEnabled;
    sInterruptsEnabled = level;
    return previous;
}

s32 OSDisableScheduler() { return sSchedulerDisabled++; }
s32 OSEnableScheduler() { return sSchedulerDisabled--; }
