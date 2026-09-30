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
// Which floating-point exceptions trap; the 3DS's VFP traps none, whatever is set here.
u32 __OSFpscrEnableBits = 0;

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
void deliverPendingInterrupts();
} // namespace

// The previous state comes back, as the SDK returns it, so nested sections restore what they found.
BOOL OSDisableInterrupts() {
    const BOOL previous = sInterruptsEnabled;
    sInterruptsEnabled = FALSE;
    return previous;
}

// Turning interrupts back on delivers what was raised while they were off, as the Wii's CPU takes them.
BOOL OSEnableInterrupts() {
    const BOOL previous = sInterruptsEnabled;
    sInterruptsEnabled = TRUE;
    deliverPendingInterrupts();
    return previous;
}

BOOL OSRestoreInterrupts(BOOL level) {
    const BOOL previous = sInterruptsEnabled;
    sInterruptsEnabled = level;
    if (level) deliverPendingInterrupts();
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
OSInterruptMask sUnmasked = 0;
u32 sHid2 = 0, sWpar = 0;
} // namespace
__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler) {
    const __OSInterruptHandler previous = sInterruptHandlers[interrupt & 31];
    sInterruptHandlers[interrupt & 31] = handler;
    return previous;
}
OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask) {
    const OSInterruptMask previous = ~sUnmasked;
    sUnmasked |= mask;
    return previous;
}
OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask) {
    const OSInterruptMask previous = ~sUnmasked;
    sUnmasked &= ~mask;
    return previous;
}
namespace {
u32 sPendingInterrupts = 0;

// The lowest-numbered source first, as the Wii's dispatcher takes them; a handler runs with interrupts off.
void deliverPendingInterrupts() {
    while (sInterruptsEnabled) {
        const u32 ready = sPendingInterrupts & sUnmasked;
        if (!ready) return;
        const int source = __builtin_clz(ready);
        sPendingInterrupts &= ~(0x80000000u >> source);
        const __OSInterruptHandler handler = sInterruptHandlers[source];
        if (!handler) continue;
        OSContext context;
        std::memset(&context, 0, sizeof context);
        sInterruptsEnabled = FALSE;
        handler(static_cast<__OSInterrupt>(source), &context);
        sInterruptsEnabled = TRUE;
    }
}
} // namespace

// A device of the port's raises one of the Wii's interrupts (the GPU's command processor, for its pixel engine).
extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt) {
    sPendingInterrupts |= 0x80000000u >> (interrupt & 31);
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
