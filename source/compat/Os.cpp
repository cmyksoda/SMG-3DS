// The Wii's OS services that need no hardware: cache controls (the renderer flushes its own buffers for the 3DS GPU;
// they only tell it which memory changed), interrupt and scheduler state, the console and panics.
#include <revolution/base/PPCArch.h>
#include <revolution/os.h>
#include <revolution/os/OSCache.h>
#include <revolution/os/OSInterrupt.h>
#include <revolution/os/OSThread.h>
#include <smg/Interrupts.hpp>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if __has_include(<3ds.h>)
#include <3ds.h>
#else
#include <mutex>
#endif

// The Wii's bus clock, which the SDK reads from low memory; OSGetTime's ticks run at a quarter of it.
u32 __OSBusClock = 243000000;
// Which floating-point exceptions trap; the 3DS's VFP traps none, whatever is set here.
u32 __OSFpscrEnableBits = 0;

// The Wii's code flushes whatever its GPU will read after the CPU wrote it, and invalidates what a DMA will fill: the
// renderer, which keeps display lists' meshes on the 3DS GPU (source/compat/GxRender.cpp), hears of every such range.
extern "C" {
void (*smgGpuMemoryChanged)(const void* start, u32 size) = nullptr;
}
void DCFlushRange(void* start, u32 size) { if (smgGpuMemoryChanged) smgGpuMemoryChanged(start, size); }
void DCStoreRange(void* start, u32 size) { if (smgGpuMemoryChanged) smgGpuMemoryChanged(start, size); }
void DCStoreRangeNoSync(void* start, u32 size) { if (smgGpuMemoryChanged) smgGpuMemoryChanged(start, size); }
void DCInvalidateRange(void* start, u32 size) { if (smgGpuMemoryChanged) smgGpuMemoryChanged(start, size); }
// dcbz zeroes whole cache lines, the one cache operation whose effect code sees. Only the bytes asked for are
// cleared: the port's heap does not keep the Wii's 32-byte alignment, so rounding out could reach a neighbour.
void DCZeroRange(void* start, u32 size) {
    std::memset(start, 0, size);
    if (smgGpuMemoryChanged) smgGpuMemoryChanged(start, size);
}
void PPCSync() { __sync_synchronize(); }

namespace {
#if __has_include(<3ds.h>)
class CpuLock {
public:
    void lock() { LightLock_Lock(&lock_); }
    bool try_lock() { return LightLock_TryLock(&lock_) == 0; }
    void unlock() { LightLock_Unlock(&lock_); }

private:
    LightLock lock_ = 1;
};
#else
using CpuLock = std::mutex;
#endif

// Interrupts off is the Wii's one CPU held: nothing else ran meanwhile, so a thread turning them off waits out any
// other thread's off-section, handler or alarm. Each thread has its own state, as each Wii thread had its MSR.
CpuLock sCpu;
thread_local bool tOff = false;
s32 sSchedulerDisabled = 0;
void deliverPendingInterrupts();
} // namespace

// The previous state comes back, as the SDK returns it, so nested sections restore what they found.
BOOL OSDisableInterrupts() {
    if (tOff) return FALSE;
    sCpu.lock();
    tOff = true;
    return TRUE;
}

// Turning interrupts back on delivers what was raised while they were off, as the Wii's CPU takes them.
BOOL OSEnableInterrupts() {
    if (!tOff) return TRUE;
    tOff = false;
    sCpu.unlock();
    deliverPendingInterrupts();
    return FALSE;
}

BOOL OSRestoreInterrupts(BOOL level) { return level ? OSEnableInterrupts() : OSDisableInterrupts(); }

smg::InterruptsReleased::InterruptsReleased() : held_(tOff) {
    if (held_) OSEnableInterrupts();
}
smg::InterruptsReleased::~InterruptsReleased() {
    if (held_) OSDisableInterrupts();
}
smg::AsInterrupt::AsInterrupt() : held_(OSDisableInterrupts()) {}
smg::AsInterrupt::~AsInterrupt() { OSRestoreInterrupts(held_); }

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

// Each SDK library announces its version at boot; the SDK prints the line, as here.
void OSRegisterVersion(const char* version) { OSReport("%s\n", version); }

// The game's handlers for CPU exceptions (JUTException's crash screen), kept per exception as the SDK keeps them.
// A fault on the 3DS is reported by the 3DS itself, so none of these is called.
#include <revolution/os/OSError.h>
namespace {
OSErrorHandler sErrorHandlers[OS_ERROR_MAX] = {};
} // namespace
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    if (error >= OS_ERROR_MAX) return nullptr;
    const OSErrorHandler previous = sErrorHandlers[error];
    sErrorHandlers[error] = handler;
    return previous;
}

// The PowerPC's machine state register, which JUTException reads and writes to arm floating-point exceptions.
// It holds a Wii game's usual value (interrupts, FPU, translation on); the 3DS's CPU takes no direction from it.
namespace {
u32 sMachineState = 0x0000B032;
} // namespace
u32 PPCMfmsr() { return sMachineState; }
void PPCMtmsr(u32 state) { sMachineState = state; }

// The Wii's interrupt handlers by source, and which sources are unmasked. The port's own devices raise the ones it
// has (the GPU's, from its command processor); the table is the SDK's, kept as the SDK keeps it.
#include <revolution/os/OSContext.h>
#include <revolution/os/OSReset.h>
namespace {
__OSInterruptHandler sInterruptHandlers[32] = {};
std::atomic<OSInterruptMask> sUnmasked{0};
u32 sHid2 = 0, sWpar = 0;
} // namespace
__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler) {
    const __OSInterruptHandler previous = sInterruptHandlers[interrupt & 31];
    sInterruptHandlers[interrupt & 31] = handler;
    return previous;
}
OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask) {
    const OSInterruptMask previous = ~sUnmasked.fetch_or(mask);
    deliverPendingInterrupts();
    return previous;
}
OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask) { return ~sUnmasked.fetch_and(~mask); }
namespace {
std::atomic<u32> sPendingInterrupts{0};

// The lowest-numbered source first, as the Wii's dispatcher takes them, with interrupts off. A thread finding the
// CPU held leaves them to its holder, who looks again after each release, so none is lost between the two.
void deliverPendingInterrupts() {
    while (!tOff && (sPendingInterrupts & sUnmasked)) {
        if (!sCpu.try_lock()) return;
        tOff = true;
        for (u32 ready; (ready = sPendingInterrupts & sUnmasked);) {
            const int source = __builtin_clz(ready);
            sPendingInterrupts.fetch_and(~(0x80000000u >> source));
            if (const __OSInterruptHandler handler = sInterruptHandlers[source]) {
                OSContext context;
                std::memset(&context, 0, sizeof context);
                handler(static_cast<__OSInterrupt>(source), &context);
            }
        }
        tOff = false;
        sCpu.unlock();
    }
}
} // namespace

// A device of the port's raises one of the Wii's interrupts; it never waits for the CPU, as it may hold the FIFO.
extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt) {
    sPendingInterrupts.fetch_or(0x80000000u >> (interrupt & 31));
    deliverPendingInterrupts();
}

// Interrupt handlers switch the floating-point context the PowerPC saves lazily; the 3DS kernel saves a thread's
// own, so there is nothing to switch.
void OSSetCurrentContext(OSContext*) {}
void OSClearContext(OSContext*) {}

// Functions to run as the console shuts down or resets. The port ends as a 3DS app does, without the Wii's
// shutdown sequence that would run them.
void OSRegisterShutdownFunction(OSShutdownFunctionInfo*) {}

// HID2 enables the write-gather pipe and WPAR aims it; the port's GX FIFO takes the writes itself.
u32 PPCMfhid2() { return sHid2; }
void PPCMthid2(u32 value) { sHid2 = value; }
void PPCMtwpar(u32 value) { sWpar = value; }
