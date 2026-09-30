// The GPU FIFO's one write point (GXVert.h, GXRegs.h), for programs without the whole-game library.
#include <cstdio>
#include <cstdlib>

extern "C" void GXWGFifoWrite() {
    std::fprintf(stderr, "The Wii GPU is not the port's: a write to GXWGFifo\n");
    std::abort();
}
