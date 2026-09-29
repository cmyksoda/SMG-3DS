#include <3ds.h>
#include <cstdio>

int runMathTests();
int runPadTests();
int runBcsvTests();

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    std::puts("SMG-3DS native validation\n");
    runMathTests();
    runPadTests();
    runBcsvTests();
    std::puts("\nPASS: math, WPad and BCSV\n\nSTART: exit");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
}
