// A bring-up app: the game's own boot (GameSystem.cpp's main) runs until it reaches something the port does not
// have yet. Each stop names itself in the emulator's log (see docs/PORTING.md, "Road to Mario").
#include <3ds.h>
#include <cstdio>

// Whether `address` follows a call in the program's code (ARM BL/BLX, or Thumb's, with bit 0 set).
static bool isReturnAddress(u32 address, u32 codeStart, u32 codeEnd) {
    if (address & 1) {
        const u32 at = address - 1;
        if (at < codeStart + 4 || at > codeEnd) return false;
        const u16* halves = reinterpret_cast<const u16*>(at);
        return ((halves[-2] & 0xF800) == 0xF000 && (halves[-1] & 0xE800) == 0xE800) || (halves[-1] & 0xFF87) == 0x4780;
    }
    if (address & 3 || address < codeStart + 4 || address > codeEnd) return false;
    const u32 call = reinterpret_cast<const u32*>(address)[-1];
    return (call & 0x0F000000) == 0x0B000000 || (call & 0xFE000000) == 0xFA000000 || (call & 0x0FFFFFF0) == 0x012FFF30;
}

// A stop names what the port lacks, not who asked; the return addresses left on the stack say (tools/stop-callers.sh).
static void logCallers() {
    extern char __start__[];
    MemInfo code, stack;
    PageInfo page;
    u32 word = 0;
    if (R_FAILED(svcQueryMemory(&code, &page, reinterpret_cast<u32>(__start__))) ||
        R_FAILED(svcQueryMemory(&stack, &page, reinterpret_cast<u32>(&word))))
        return;
    const u32* at = &word;
    const u32* end = reinterpret_cast<const u32*>(stack.base_addr + stack.size);
    std::fputs("mario-probe: callers on the stack:", stderr);
    for (int found = 0, scanned = 0; at < end && found < 32 && scanned < 0x4000; ++at, ++scanned)
        if (isReturnAddress(*at, code.base_addr, code.base_addr + code.size)) std::fprintf(stderr, " %08lx", *at), ++found;
    std::fputc('\n', stderr);
}

// Every stop ends in abort, on any thread (linked with --wrap=abort; newlib's signal handlers are per thread). libctru's
// exit would unmount the SD card under the game's own threads (the DVD worker, mid-read), so a stop ends at once.
extern "C" [[noreturn]] void __wrap_abort() {
    logCallers();
    svcExitProcess();
}

// The game's static initializers run before main; stderr has to reach the emulator's log before any of them.
__attribute__((constructor(101))) static void logFromTheStart() {
    consoleDebugInit(debugDevice_SVC);
    std::fprintf(stderr, "mario-probe: static initialization begins\n");
}

void smgGameMain();

int main() {
    std::fprintf(stderr, "mario-probe: main\n");
    osSetSpeedupEnable(true);  // the New 3DS's 804 MHz clock and L2 cache; 268 MHz without
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);  // the top screen is the game's
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
