// The GPU FIFO's write points (GXVert.h, GXRegs.h), for programs without the whole-game library.
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
void smgGXFifo_u8(u8) { unported(); }
void smgGXFifo_s8(s8) { unported(); }
void smgGXFifo_u16(u16) { unported(); }
void smgGXFifo_s16(s16) { unported(); }
void smgGXFifo_u32(u32) { unported(); }
void smgGXFifo_s32(s32) { unported(); }
void smgGXFifo_f32(f32) { unported(); }
}
