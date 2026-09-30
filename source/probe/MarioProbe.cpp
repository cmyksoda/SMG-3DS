// A bring-up app: the game's own MarioActor, made in the native scene, runs until it reaches something the port
// does not have yet. Each stop names itself in the emulator's log (see docs/PORTING.md, "Road to Mario").
#include <3ds.h>
#include <smg/Scene.hpp>
#include <Game/Player/MarioActor.hpp>
#include <cstdio>

int main() {
    consoleDebugInit(debugDevice_SVC);
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    std::printf("MarioActor bring-up\n\n");
    std::fprintf(stderr, "mario-probe: constructing MarioActor\n");
    smg::NativeScene scene;
    auto* mario = new MarioActor("Mario");
    std::fprintf(stderr, "mario-probe: MarioActor constructed at %p\n", static_cast<void*>(mario));
    std::printf("MarioActor constructed.\nSTART exits.\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
