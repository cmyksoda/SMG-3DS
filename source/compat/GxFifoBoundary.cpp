// The GPU FIFO's write points (smg/GxFifoWrite.h), for programs without the whole-game library.
#include <revolution/types.h>
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void unported() {
    std::fprintf(stderr, "The Wii GPU is not the port's: a write to its FIFO\n");
    std::abort();
}
} // namespace

extern "C" {
unsigned char* smgFifoAt = nullptr;  // nothing gathers: every write takes the slow way
unsigned char* smgFifoEnd = nullptr;
void* smgFifoThread = nullptr;
void smgGXFifoSlow(unsigned, unsigned) { unported(); }
}
