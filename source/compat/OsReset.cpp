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
    std::exit(0);
}

// Called by the port's retrace thread (source/compat/Video.cpp) each VBlank, before the game's retrace callbacks.
void smgServiceApplet() {
    if (aptMainLoop() || sClosing) return;
    sClosing = true;
    const BOOL enabled = OSDisableInterrupts();
    const OSPowerCallback callback = sPowerCallback;
    OSRestoreInterrupts(enabled);
    if (callback) {
        callback();
    } else {
        std::fputs("The 3DS asked the app to close before the game could hear it\n", stderr);
        std::exit(0);
    }
}
}
