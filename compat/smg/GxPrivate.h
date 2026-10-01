#pragma once
// Internal GX functions some of the SDK's C sources call undeclared, as C allows and C++ does not. Built as C++
// (they include C++ headers), those files take these prototypes, as their definitions give them.
#include <revolution/gx.h>
#ifdef __cplusplus
extern "C" {
#endif
void GXResetWriteGatherPipe(void);
u32 GXGetFifoCount(const GXFifoObj* fifo);
// The port's memory for the GPU's register blocks (0 PI, 1 CP, 2 PE, 3 MEM), where GXInit points GX.
volatile void* smgGXRegisters(int unit);
// The DSP block, whose mailboxes and AI DMA are functions (source/compat/WiiAudio.cpp); JAudio2's handler writes it.
volatile void* smgAudioRegisters(void);
// GXPeekARGB and GXPeekZ's pixel (source/compat/GxFifo.cpp): 0xAARRGGBB, or Z with nearer smaller.
u32 smgPeekEfb(u16 x, u16 y, int depth);
// Any other Wii hardware block: touching it stops the program by name.
volatile void* smgUnportedRegisters(const char* block);
#ifdef __cplusplus
}
#endif
// The SDK's own internals, defined in its GX sources, which are built as C++.
void __GXSetDirtyState(void);
void __GXSendFlushPrim(void);
void __GXCleanGPFifo(void);
void __GXInitRevisionBits(void);
// A colour as the word the Wii's load of its bytes gave, R on top; the SDK reads GXColors through u32 casts.
static inline u32 smgColorWord(GXColor c) { return (u32)c.r << 24 | (u32)c.g << 16 | (u32)c.b << 8 | c.a; }
// A TEV preset's register word, field by field: the SDK's tables are bitfields, which Metrowerks packed from the top bit
// and GCC on ARM packs from the bottom.
#include <private/tev_reg.h>
static inline u32 smgTevColorWord(const tev_color_env_t* e) {
    return (u32)e->rid << 24 | (u32)e->dest << 22 | (u32)e->shift << 20 | (u32)e->clamp << 19 | (u32)e->sub << 18 | (u32)e->bias << 16 |
           (u32)e->sela << 12 | (u32)e->selb << 8 | (u32)e->selc << 4 | (u32)e->seld;
}
static inline u32 smgTevAlphaWord(const tev_alpha_env_t* e) {
    return (u32)e->rid << 24 | (u32)e->dest << 22 | (u32)e->shift << 20 | (u32)e->clamp << 19 | (u32)e->sub << 18 | (u32)e->bias << 16 |
           (u32)e->sela << 13 | (u32)e->selb << 10 | (u32)e->selc << 7 | (u32)e->seld << 4 | (u32)e->swap << 2 | (u32)e->mode;
}
