#include <3ds.h>
#include <cstdio>

int runMathTests();
int runPadTests();
int runBcsvTests();
int runModelTests();
int runCombinerTests();
int runGravityTests();
int runEtc1Tests();
int runGpuTests();
int runNameObjTests();
int runSceneTests();
int runCollisionTests();

int main() {
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    std::puts("SMG-3DS native validation\n");
    runMathTests();
    runPadTests();
    runBcsvTests();
    runModelTests();
    runCombinerTests();
    runGravityTests();
    runNameObjTests();
    runSceneTests();
    runCollisionTests();
    runEtc1Tests();
    runGpuTests();
    std::puts("\nPASS: math, WPad, BCSV, J3D, TEV, gravity, NameObj, scene, collision, ETC1\n\nSTART: exit");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
}
