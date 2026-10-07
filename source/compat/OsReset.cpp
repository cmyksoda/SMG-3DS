// The Wii's reset and power switches, as a 3DS has them. There is no reset button, so it is never pressed. The system
// owns HOME, sleep and power: each retrace services its applet events first, so the game waits while suspended, and a
// request to close (power off, or closing from HOME) reaches the game as the Wii's power button did, so it ends
// through its own power-off sequence.
#include <3ds.h>
#include <revolution/os.h>
#include <revolution/os/OSReset.h>
#include <revolution/os/OSResetSW.h>
#include <cstdio>
#include <cstdlib>

namespace {
OSPowerCallback sPowerCallback = nullptr;
bool sClosing = false;

// Not exit: libctru's unmaps the heaps the game's other threads and the GPU still use, and HOME's close then waited for
// good on a real 3DS. The DSP stops reading our buffers, APT hears the farewell HOME waits for, and the kernel ends the rest.
[[noreturn]] void endApp() {
    ndspExit();
    aptExit();
    svcExitProcess();
}

// The game's own power-off gets a moment (a save in progress finishes); past it the app ends anyway.
void endSoon(void*) {
    svcSleepThread(3'000'000'000);
    std::fputs("The game had not powered off 3 s after the 3DS asked it to close; the app ends now\n", stderr);
    endApp();
}
} // namespace

extern "C" {
OSPowerCallback OSSetPowerCallback(OSPowerCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    const OSPowerCallback previous = sPowerCallback;
    sPowerCallback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}

BOOL OSGetResetButtonState() { return FALSE; }

void OSShutdownSystem() {
    std::fputs("The game powered off; the app ends as the 3DS asked\n", stderr);
    endApp();
}

// Called by the port's retrace thread (source/compat/Video.cpp) each VBlank, before the game's retrace callbacks.
void smgServiceApplet() {
    if (aptMainLoop() || sClosing) return;
    sClosing = true;
    threadCreate(endSoon, nullptr, 0x1000, 0x18, -2, true);
    const BOOL enabled = OSDisableInterrupts();
    const OSPowerCallback callback = sPowerCallback;
    OSRestoreInterrupts(enabled);
    if (callback) {
        callback();
    } else {
        std::fputs("The 3DS asked the app to close before the game could hear it\n", stderr);
        endApp();
    }
}
}
