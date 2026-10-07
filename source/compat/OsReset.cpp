// The Wii's reset and power switches, as a 3DS has them. There is no reset button, so it is never pressed. The system
// owns HOME, sleep and power: each retrace services its applet events first, so the game waits while suspended, and a
// request to close (power off, or closing from HOME) ends the app at once: the game's own power-off needs the GPU,
// which HOME holds, and HOME reports an error when the app is slow to go.
#include <3ds.h>
#include <revolution/os.h>
#include <revolution/os/OSReset.h>
#include <revolution/os/OSResetSW.h>
#include <cstdio>
#include <cstdlib>

extern "C" void smgNandHold();  // source/compat/Nand.cpp

namespace {
OSPowerCallback sPowerCallback = nullptr;
bool sClosing = false;

// Not exit: libctru's unmaps the heaps the game's other threads and the GPU still use, and HOME's close then waited for
// good on a real 3DS. The DSP stops reading our buffers, APT hears the farewell HOME waits for, and the kernel ends the rest.
[[noreturn]] void endApp() {
    smgNandHold();
    ndspExit();
    aptExit();
    svcExitProcess();
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
    std::fputs("The 3DS asked the app to close; it ends now\n", stderr);
    endApp();
}
}
