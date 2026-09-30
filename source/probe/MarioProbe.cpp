// A bring-up app: the game's own boot (GameSystem.cpp's main) runs until it reaches something the port does not
// have yet. Each stop names itself in the emulator's log (see docs/PORTING.md, "Road to Mario").
#include <3ds.h>
#include <cstdio>

// The game's static initializers run before main; stderr has to reach the emulator's log before any of them.
__attribute__((constructor(101))) static void logFromTheStart() {
    consoleDebugInit(debugDevice_SVC);
    std::fprintf(stderr, "mario-probe: static initialization begins\n");
}

void smgGameMain();

int main() {
    std::fprintf(stderr, "mario-probe: main\n");
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    // consoleInit takes stderr for the screen too; stops belong in the log, which outlives the app.
    consoleDebugInit(debugDevice_SVC);
    std::printf("SMG boot bring-up\n");
    std::fprintf(stderr, "mario-probe: application memory %lu KiB, %lu KiB free\n", osGetMemRegionSize(MEMREGION_APPLICATION) / 1024,
                 osGetMemRegionFree(MEMREGION_APPLICATION) / 1024);
    std::fprintf(stderr, "mario-probe: entering the game's main\n");
    smgGameMain();
    gfxExit();
    return 0;
}
