// The Wii GPU's side of the whole game: the SDK's own GX writes the GPU's registers and FIFO, and these are
// where they go. The registers are memory the GX library reads back; the FIFO's command processor, which will
// turn the Wii's commands into citro3d's, is not written yet, so the first command stops the program by name.
#include <revolution/types.h>
#include <cstdio>
#include <cstdlib>

namespace {
// The processor interface, command processor, pixel engine and memory interface blocks, 4 KiB each on the Wii.
alignas(32) u8 sRegisters[4][0x1000];
} // namespace

extern "C" volatile void* smgGXRegisters(int unit) { return sRegisters[unit]; }

extern "C" volatile void* smgUnportedRegisters(const char* block) {
    std::fprintf(stderr, "The Wii's %s hardware registers are not the port's\n", block);
    std::abort();
}

extern "C" void GXWGFifoWrite() {
    std::fprintf(stderr, "The Wii GPU's command processor is not the port's yet: GX wrote its first FIFO command\n");
    std::abort();
}
