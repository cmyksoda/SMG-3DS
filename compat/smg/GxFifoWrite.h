#ifndef SMG_GX_FIFO_WRITE_H
#define SMG_GX_FIFO_WRITE_H
// The GPU FIFO's write points (GXRegs.h's GX_WRITE_*, GXVert.h's GXWGFIFO_PUT), big-endian as the Wii's pipe takes
// them: inline stores into the main GX thread's gather buffer; another thread or a full buffer goes the slow way
// (source/compat/GxFifo.cpp; programs without the whole game stop at their first write).
#ifdef __cplusplus
extern "C" {
#endif
// One struct, so a single address reaches the write point, the end of its room and the game thread.
struct SmgGXFifo {
    unsigned char* at;
    unsigned char* end;
    void* thread;
};
extern struct SmgGXFifo smgFifo;
void smgGXFifoSlow(unsigned value, unsigned bytes);

// Host tests have no thread register: they define this.
#if defined(__arm__) && !defined(SMG_GX_FIFO_SELF)
#define SMG_GX_FIFO_SELF(self) __asm__("mrc p15, 0, %0, c13, c0, 3" : "=r"(self))
#endif

// Whether this is the game thread and `bytes` fit at its write point, `at`. A command that does not fit goes piece by
// piece the slow way, so the ring turns over and hands over where a look for room before every piece would have.
static inline int smgGXFifoRoom(unsigned bytes, unsigned char** at) {
#ifdef SMG_GX_FIFO_SELF
    void* self;
    SMG_GX_FIFO_SELF(self);
    *at = smgFifo.at;
    return self == smgFifo.thread && *at + bytes <= smgFifo.end;
#else
    (void)bytes, (void)at;
    return 0;
#endif
}

// Word and halfword stores: the 3DS takes them unaligned.
static inline unsigned char* smgGXFifoStore(unsigned char* at, unsigned value, unsigned bytes) {
    if (bytes == 4) {
        const unsigned word = __builtin_bswap32(value);
        __builtin_memcpy(at, &word, 4);
    } else if (bytes == 2) {
        const unsigned short half = __builtin_bswap16((unsigned short)value);
        __builtin_memcpy(at, &half, 2);
    } else {
        *at = (unsigned char)value;
    }
    return at + bytes;
}

static inline void smgGXFifoPut(unsigned value, unsigned bytes) {
    unsigned char* at;
    if (smgGXFifoRoom(bytes, &at)) {
        smgFifo.at = smgGXFifoStore(at, value, bytes);
        return;
    }
    smgGXFifoSlow(value, bytes);
}
static inline unsigned smgGXFifoBits(float v) {
    union {
        float f;
        unsigned u;
    } bits;
    bits.f = v;
    return bits.u;
}
static inline void smgGXFifo_u8(unsigned char v) { smgGXFifoPut(v, 1); }
static inline void smgGXFifo_s8(signed char v) { smgGXFifoPut((unsigned char)v, 1); }
static inline void smgGXFifo_u16(unsigned short v) { smgGXFifoPut(v, 2); }
static inline void smgGXFifo_s16(short v) { smgGXFifoPut((unsigned short)v, 2); }
static inline void smgGXFifo_u32(unsigned long v) { smgGXFifoPut((unsigned)v, 4); }
static inline void smgGXFifo_s32(long v) { smgGXFifoPut((unsigned)v, 4); }
static inline void smgGXFifo_f32(float v) { smgGXFifoPut(smgGXFifoBits(v), 4); }

// A command's pieces (each a value and its size in bytes) with one look for room, not one per piece.
static inline void smgGXFifoPut2(unsigned a, unsigned aBytes, unsigned b, unsigned bBytes) {
    unsigned char* at;
    if (smgGXFifoRoom(aBytes + bBytes, &at)) {
        smgFifo.at = smgGXFifoStore(smgGXFifoStore(at, a, aBytes), b, bBytes);
        return;
    }
    smgGXFifoSlow(a, aBytes), smgGXFifoSlow(b, bBytes);
}
static inline void smgGXFifoPut3(unsigned a, unsigned aBytes, unsigned b, unsigned bBytes, unsigned c,
                                 unsigned cBytes) {
    unsigned char* at;
    if (smgGXFifoRoom(aBytes + bBytes + cBytes, &at)) {
        smgFifo.at = smgGXFifoStore(smgGXFifoStore(smgGXFifoStore(at, a, aBytes), b, bBytes), c, cBytes);
        return;
    }
    smgGXFifoSlow(a, aBytes), smgGXFifoSlow(b, bBytes), smgGXFifoSlow(c, cBytes);
}
static inline void smgGXFifoPut4(unsigned a, unsigned aBytes, unsigned b, unsigned bBytes, unsigned c, unsigned cBytes,
                                 unsigned d, unsigned dBytes) {
    unsigned char* at;
    if (smgGXFifoRoom(aBytes + bBytes + cBytes + dBytes, &at)) {
        at = smgGXFifoStore(smgGXFifoStore(at, a, aBytes), b, bBytes);
        smgFifo.at = smgGXFifoStore(smgGXFifoStore(at, c, cBytes), d, dBytes);
        return;
    }
    smgGXFifoSlow(a, aBytes), smgGXFifoSlow(b, bBytes), smgGXFifoSlow(c, cBytes), smgGXFifoSlow(d, dBytes);
}

// An XF load of `count` floats, and of a 3x4 matrix's upper left 3x3 (GXLoadPosMtxImm and GXLoadNrmMtxImm): inline when
// it fits; GxFifo.cpp finds room for the whole load (the first) or goes a piece at a time (the second), as before.
void smgGXFifoXfLoadSlow(unsigned reg, const float* values, unsigned count);
void smgGXFifoXfLoadNrmSlow(unsigned reg, const float* mtx);

static inline unsigned smgGXFifoWordOf(const float* value) {
    unsigned word;
    __builtin_memcpy(&word, value, 4);
    return word;
}
static inline void smgGXFifoXfLoad(unsigned reg, const float* values, unsigned count) {
    unsigned char* at;
    if (smgGXFifoRoom(5 + 4 * count, &at)) {
        at = smgGXFifoStore(smgGXFifoStore(at, 0x10, 1), reg, 4);
        for (unsigned i = 0; i < count; ++i) at = smgGXFifoStore(at, smgGXFifoWordOf(values + i), 4);
        smgFifo.at = at;
        return;
    }
    smgGXFifoXfLoadSlow(reg, values, count);
}
static inline void smgGXFifoXfLoadNrm(unsigned reg, const float* mtx) {
    unsigned char* at;
    if (smgGXFifoRoom(5 + 4 * 9, &at)) {
        at = smgGXFifoStore(smgGXFifoStore(at, 0x10, 1), reg, 4);
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned column = 0; column < 3; ++column)
                at = smgGXFifoStore(at, smgGXFifoWordOf(mtx + 4 * row + column), 4);
        smgFifo.at = at;
        return;
    }
    smgGXFifoXfLoadNrmSlow(reg, mtx);
}

// Each type's size in the stream, and its value as the word the store takes (GXVert.h's vertex writers).
#define SMG_GX_FIFO_BYTES_u8 1
#define SMG_GX_FIFO_BYTES_s8 1
#define SMG_GX_FIFO_BYTES_u16 2
#define SMG_GX_FIFO_BYTES_s16 2
#define SMG_GX_FIFO_BYTES_u32 4
#define SMG_GX_FIFO_BYTES_f32 4
#define SMG_GX_FIFO_BITS_u8(v) ((unsigned)(unsigned char)(v))
#define SMG_GX_FIFO_BITS_s8(v) ((unsigned)(unsigned char)(v))
#define SMG_GX_FIFO_BITS_u16(v) ((unsigned)(unsigned short)(v))
#define SMG_GX_FIFO_BITS_s16(v) ((unsigned)(unsigned short)(v))
#define SMG_GX_FIFO_BITS_u32(v) ((unsigned)(v))
#define SMG_GX_FIFO_BITS_f32(v) smgGXFifoBits(v)
#ifdef __cplusplus
}

// GXVert.h includes this inside its extern "C", where templates cannot be.
extern "C++" {
// Commands of any length with one look for room: `pieces` writes to an output that stores inline or, if `bytes` do
// not fit, sends each piece the slow way, as before.
class SmgGXFifoStores {
public:
    explicit SmgGXFifoStores(unsigned char* at) : at_(at) {}
    void u8(unsigned v) { at_ = smgGXFifoStore(at_, v, 1); }
    void u16(unsigned v) { at_ = smgGXFifoStore(at_, v, 2); }
    void u32(unsigned v) { at_ = smgGXFifoStore(at_, v, 4); }
    void f32(float v) { at_ = smgGXFifoStore(at_, smgGXFifoBits(v), 4); }
    void bp(unsigned reg) { u8(0x61), u32(reg); }
    unsigned char* at() const { return at_; }

private:
    unsigned char* at_;
};

struct SmgGXFifoSlowWrites {
    void u8(unsigned v) const { smgGXFifoSlow(v, 1); }
    void u16(unsigned v) const { smgGXFifoSlow(v, 2); }
    void u32(unsigned v) const { smgGXFifoSlow(v, 4); }
    void f32(float v) const { smgGXFifoSlow(smgGXFifoBits(v), 4); }
    void bp(unsigned reg) const { u8(0x61), u32(reg); }
};

template <class Pieces> static inline void smgGXFifoRun(unsigned bytes, Pieces pieces) {
    unsigned char* at;
    if (smgGXFifoRoom(bytes, &at)) {
        SmgGXFifoStores out(at);
        pieces(out);
        smgFifo.at = out.at();
        return;
    }
    SmgGXFifoSlowWrites out;
    pieces(out);
}
}
#endif
#endif
