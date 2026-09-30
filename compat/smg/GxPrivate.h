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
