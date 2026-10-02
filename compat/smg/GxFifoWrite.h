#ifndef SMG_GX_FIFO_WRITE_H
#define SMG_GX_FIFO_WRITE_H
// The GPU FIFO's write points (GXRegs.h's GX_WRITE_*, GXVert.h's GXWGFIFO_PUT), big-endian as the Wii's pipe takes
// them: inline stores into the main GX thread's gather buffer; another thread or a full buffer goes the slow way
// (source/compat/GxFifo.cpp; programs without the whole game stop at their first write).
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned char* smgFifoAt;
extern unsigned char* smgFifoEnd;
extern void* smgFifoThread;
void smgGXFifoSlow(unsigned value, unsigned bytes);

static inline void smgGXFifoPut(unsigned value, unsigned bytes) {
#if defined(__arm__)
    void* self;
    __asm__("mrc p15, 0, %0, c13, c0, 3" : "=r"(self));  // the thread's local storage: which thread this is
    unsigned char* at = smgFifoAt;
    if (self == smgFifoThread && at + bytes <= smgFifoEnd) {
        for (unsigned i = 0; i < bytes; ++i) at[i] = (unsigned char)(value >> (8 * (bytes - 1 - i)));
        smgFifoAt = at + bytes;
        return;
    }
#endif
    smgGXFifoSlow(value, bytes);
}
static inline void smgGXFifo_u8(unsigned char v) { smgGXFifoPut(v, 1); }
static inline void smgGXFifo_s8(signed char v) { smgGXFifoPut((unsigned char)v, 1); }
static inline void smgGXFifo_u16(unsigned short v) { smgGXFifoPut(v, 2); }
static inline void smgGXFifo_s16(short v) { smgGXFifoPut((unsigned short)v, 2); }
static inline void smgGXFifo_u32(unsigned long v) { smgGXFifoPut((unsigned)v, 4); }
static inline void smgGXFifo_s32(long v) { smgGXFifoPut((unsigned)v, 4); }
static inline void smgGXFifo_f32(float v) {
    union {
        float f;
        unsigned u;
    } bits;
    bits.f = v;
    smgGXFifoPut(bits.u, 4);
}
#ifdef __cplusplus
}
#endif
#endif
