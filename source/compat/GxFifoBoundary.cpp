// The GPU FIFO's write points (smg/GxFifoWrite.h), for programs without the whole-game library.
#include <revolution/types.h>
#include <smg/GxFifoWrite.h>
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void unported() {
    std::fprintf(stderr, "The Wii GPU is not the port's: a write to its FIFO\n");
    std::abort();
}
} // namespace

extern "C" {
SmgGXFifo smgFifo = {nullptr, nullptr, nullptr};  // nothing gathers: every write takes the slow way
void smgGXFifoSlow(unsigned, unsigned) { unported(); }
void smgGXLoadIndexedNow(unsigned, unsigned, unsigned) { unported(); }
void smgGXFifoXfLoadSlow(unsigned, const float*, unsigned) { unported(); }
void smgGXFifoXfLoadNrmSlow(unsigned, const float*) { unported(); }
void smgGXCallDisplayListNow(const void*, u32) { unported(); }
// No frames in flight without the whole game: every write swaps, as on the Wii.
int smgSwapSinceDrawn(const void*) { return 1; }
}
