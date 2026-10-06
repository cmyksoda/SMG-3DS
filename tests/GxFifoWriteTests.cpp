// smg/GxFifoWrite.h's writers that send several pieces with one look for room, against the same pieces one at a time
// and against the bytes written out by hand: the same stream, and the ring handed over and turned over at the same
// places, from every write point near a hand-over or the ring's end. GxFifo.cpp itself needs libctru, so the slow way
// here is a model of its makeRoom: hand over what was written, skip to the ring's start when its end is near.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void* sSelf = nullptr;
}
#define SMG_GX_FIFO_SELF(self) ((self) = sSelf)
#include <smg/GxFifoWrite.h>

namespace {
constexpr unsigned RingBytes = 512, Chunk = 128;
unsigned char sRing[RingBytes];
unsigned char* sPublishFrom = sRing;
char sGameThread, sOtherThread;

struct Handover {
    std::vector<unsigned char> stream;  // what was handed over, in order
    std::vector<int> pieces;            // each hand-over's size, a skip as its negative
    std::vector<unsigned char> other;   // what another thread wrote
    std::vector<unsigned> slowLoads;    // (reg, count) of each XF load sent the slow way
    unsigned slowCalls = 0;
    bool operator==(const Handover& o) const { return stream == o.stream && pieces == o.pieces && other == o.other && slowLoads == o.slowLoads; }
} sLog;

void publish() {
    const int size = static_cast<int>(smgFifo.at - sPublishFrom);
    if (!size) return;
    sLog.stream.insert(sLog.stream.end(), sPublishFrom, smgFifo.at);
    sLog.pieces.push_back(size);
    sPublishFrom = smgFifo.at;
}

void makeRoom(unsigned bytes) {
    publish();
    for (;;) {
        const unsigned toEnd = RingBytes - static_cast<unsigned>(smgFifo.at - sRing);
        if (toEnd >= bytes) {
            smgFifo.end = smgFifo.at + (toEnd < Chunk ? toEnd : Chunk);
            return;
        }
        sLog.pieces.push_back(-static_cast<int>(toEnd));
        smgFifo.at = sPublishFrom = sRing;
    }
}
} // namespace

extern "C" {
SmgGXFifo smgFifo = {sRing, sRing, nullptr};
void smgGXFifoSlow(unsigned value, unsigned bytes) {
    ++sLog.slowCalls;
    unsigned char order[4];
    for (unsigned i = 0; i < bytes; ++i) order[i] = static_cast<unsigned char>(value >> (8 * (bytes - 1 - i)));
    if (sSelf != smgFifo.thread) return void(sLog.other.insert(sLog.other.end(), order, order + bytes));
    if (smgFifo.at + bytes > smgFifo.end) makeRoom(bytes);
    std::memcpy(smgFifo.at, order, bytes);
    smgFifo.at += bytes;
}
void smgGXFifoXfLoadSlow(unsigned reg, const float*, unsigned count) { sLog.slowLoads.push_back(reg), sLog.slowLoads.push_back(count); }
void smgGXFifoXfLoadNrmSlow(unsigned reg, const float*) { sLog.slowLoads.push_back(reg), sLog.slowLoads.push_back(~0u); }
}

namespace {
unsigned checks = 0;
void check(bool pass, const char* message, unsigned offset, unsigned room) {
    ++checks;
    if (!pass) {
        std::fprintf(stderr, "FAIL: %s (write point %u, room %u)\n", message, offset, room);
        std::exit(1);
    }
}

// The game thread's write point at `offset`, `room` bytes before the end of what it may store, or another thread's.
void start(unsigned offset, unsigned room, bool game) {
    sLog = Handover();
    smgFifo.thread = &sGameThread;
    sSelf = game ? &sGameThread : &sOtherThread;
    smgFifo.at = sPublishFrom = sRing + offset;
    smgFifo.end = smgFifo.at + room;
    std::memset(sRing, 0xEE, sizeof sRing);
}

struct Piece {
    unsigned value, bytes;
};
using Pieces = std::vector<Piece>;

std::vector<unsigned char> bytesOf(const Pieces& pieces) {
    std::vector<unsigned char> out;
    for (const Piece& p : pieces)
        for (unsigned i = 0; i < p.bytes; ++i) out.push_back(static_cast<unsigned char>(p.value >> (8 * (p.bytes - 1 - i))));
    return out;
}

// `write` sends `pieces` as one fused call; the reference sends them one at a time. Both from the same write point.
template <class Write> void compare(const char* name, const Pieces& pieces, Write write) {
    const std::vector<unsigned char> expected = bytesOf(pieces);
    // Every write point where a hand-over, or the ring's end, is near, and every room: the pieces start anywhere.
    for (unsigned offset = 0; offset < RingBytes; offset += (offset < 8 || offset + 100 >= RingBytes ? 1 : 97)) {
        for (unsigned room = 0; room <= 90 && offset + room <= RingBytes; ++room) {
            for (const bool game : {true, false}) {
                start(offset, room, game);
                write();
                publish();
                const Handover fused = sLog;
                start(offset, room, game);
                for (const Piece& p : pieces) smgGXFifoPut(p.value, p.bytes);
                publish();
                check(fused == sLog, name, offset, room);
                check(game ? fused.stream.size() == expected.size() && fused.stream == expected : fused.other == expected, name, offset, room);
            }
        }
    }
}

void commands() {
    for (unsigned v = 0; v < 4; ++v) {
        const unsigned a = 0x12345678u * (v + 1), b = 0x9ABCDEF0u ^ v, c = 0x0F1E2D3Cu + v;
        compare("a BP load", {{0x61, 1}, {a, 4}}, [&] { smgGXFifoPut2(0x61, 1, a, 4); });
        compare("a CP load", {{0x08, 1}, {a & 0xFF, 1}, {b, 4}}, [&] { smgGXFifoPut3(0x08, 1, a & 0xFF, 1, b, 4); });
        compare("an XF register load", {{0x10, 1}, {a, 4}, {b, 4}}, [&] { smgGXFifoPut3(0x10, 1, a, 4, b, 4); });
        compare("an XF header", {{0x10, 1}, {a & 0xFFFF, 2}, {b & 0xFFFF, 2}}, [&] { smgGXFifoPut3(0x10, 1, a & 0xFFFF, 2, b & 0xFFFF, 2); });
        compare("a primitive's header", {{0x98, 1}, {a & 0xFFFF, 2}}, [&] { smgGXFifoPut2(0x98, 1, a & 0xFFFF, 2); });
        compare("two BP loads", {{0x61, 1}, {a, 4}, {0x61, 1}, {b, 4}}, [&] { smgGXFifoPut4(0x61, 1, a, 4, 0x61, 1, b, 4); });
        compare("three words", {{a, 4}, {b, 4}, {c, 4}}, [&] { smgGXFifoPut3(a, 4, b, 4, c, 4); });
        compare("four halves", {{a & 0xFFFF, 2}, {b & 0xFFFF, 2}, {c & 0xFFFF, 2}, {a >> 16, 2}},
                [&] { smgGXFifoPut4(a & 0xFFFF, 2, b & 0xFFFF, 2, c & 0xFFFF, 2, a >> 16, 2); });
        compare("four bytes", {{a & 0xFF, 1}, {b & 0xFF, 1}, {c & 0xFF, 1}, {a >> 24, 1}}, [&] { smgGXFifoPut4(a & 0xFF, 1, b & 0xFF, 1, c & 0xFF, 1, a >> 24, 1); });
    }
}

void runs() {
    const float f = -2.5f;
    unsigned fBits;
    std::memcpy(&fBits, &f, 4);
    for (unsigned v = 0; v < 3; ++v) {
        const unsigned a = 0xA1B2C3D4u + v, b = 0x01020304u * (v + 1);
        const Pieces pieces = {{0x61, 1}, {a, 4}, {0x10, 1}, {a & 0xFFFF, 2}, {b & 0xFFFF, 2}, {fBits, 4}, {b, 4}, {0x08, 1}, {a & 0xFF, 1}, {0, 4}};
        compare("a run of commands", pieces, [&] {
            smgGXFifoRun(29, [&](auto& out) {
                out.bp(a);
                out.u8(0x10), out.u16(a & 0xFFFF), out.u16(b & 0xFFFF);
                out.f32(f), out.u32(b);
                out.u8(0x08), out.u8(a & 0xFF), out.u32(0);
            });
        });
    }
}

// Each type's size and word, as GXVert.h's writers use them.
void vertexTypes() {
    check(SMG_GX_FIFO_BYTES_u8 == 1 && SMG_GX_FIFO_BYTES_s8 == 1 && SMG_GX_FIFO_BYTES_u16 == 2 && SMG_GX_FIFO_BYTES_s16 == 2 &&
              SMG_GX_FIFO_BYTES_u32 == 4 && SMG_GX_FIFO_BYTES_f32 == 4, "each vertex type has its size", 0, 0);
    check(SMG_GX_FIFO_BITS_s8((signed char)-2) == 0xFE && SMG_GX_FIFO_BITS_u8((unsigned char)200) == 200, "a byte is its low byte", 0, 0);
    check(SMG_GX_FIFO_BITS_s16((short)-2) == 0xFFFE && SMG_GX_FIFO_BITS_u16((unsigned short)0xABCD) == 0xABCD, "a half is its low half", 0, 0);
    check(SMG_GX_FIFO_BITS_f32(1.0f) == 0x3F800000u && SMG_GX_FIFO_BITS_f32(-2.0f) == 0xC0000000u, "a float is its bits", 0, 0);
    // A position of three signed halves, as GXPosition3s16 sends it.
    start(0, 64, true);
    smgGXFifoPut3(SMG_GX_FIFO_BITS_s16((short)-2), SMG_GX_FIFO_BYTES_s16, SMG_GX_FIFO_BITS_s16((short)258), SMG_GX_FIFO_BYTES_s16, SMG_GX_FIFO_BITS_s16((short)7),
                  SMG_GX_FIFO_BYTES_s16);
    publish();
    const unsigned char expected[] = {0xFF, 0xFE, 0x01, 0x02, 0x00, 0x07};
    check(sLog.stream.size() == 6 && std::memcmp(sLog.stream.data(), expected, 6) == 0, "three signed halves go in as the GPU takes them", 0, 64);
}

// An XF load fits or goes to GxFifo.cpp whole; where it fits, its words are as the pieces would be.
void matrices() {
    float values[16];
    for (unsigned i = 0; i < 16; ++i) values[i] = static_cast<float>(i) * 1.5f - 7.0f;
    for (unsigned count : {1u, 8u, 12u, 16u}) {
        Pieces pieces = {{0x10, 1}, {0x40BAD, 4}};
        for (unsigned i = 0; i < count; ++i) pieces.push_back({SMG_GX_FIFO_BITS_f32(values[i]), 4});
        const std::vector<unsigned char> expected = bytesOf(pieces);
        for (unsigned offset = 0; offset < RingBytes; offset += 13) {
            for (unsigned room = 0; room <= 100 && offset + room <= RingBytes; ++room) {
                start(offset, room, true);
                smgGXFifoXfLoad(0x40BAD, values, count);
                const bool fits = room >= expected.size();
                check(fits == sLog.slowLoads.empty(), "an XF load goes slow exactly when it does not fit", offset, room);
                if (!fits) {
                    check(sLog.slowLoads.size() == 2 && sLog.slowLoads[0] == 0x40BAD && sLog.slowLoads[1] == count && smgFifo.at == sRing + offset,
                          "an XF load that does not fit is left whole to GxFifo.cpp", offset, room);
                    continue;
                }
                check(smgFifo.at == sRing + offset + expected.size() && std::memcmp(sRing + offset, expected.data(), expected.size()) == 0, "an XF load's bytes", offset, room);
                start(offset, room, false);
                smgGXFifoXfLoad(0x40BAD, values, count);
                check(sLog.slowLoads.size() == 2 && smgFifo.at == sRing + offset, "another thread's XF load goes slow", offset, room);
            }
        }
    }
    // The upper left 3x3 of a 3x4, a row of three at a time.
    Pieces nrm = {{0x10, 1}, {0x40BAD, 4}};
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column) nrm.push_back({SMG_GX_FIFO_BITS_f32(values[4 * row + column]), 4});
    const std::vector<unsigned char> expected = bytesOf(nrm);
    for (unsigned room = 0; room <= 60; ++room) {
        start(3, room, true);
        smgGXFifoXfLoadNrm(0x40BAD, values);
        const bool fits = room >= expected.size();
        check(fits == sLog.slowLoads.empty(), "a normal matrix load goes slow exactly when it does not fit", 3, room);
        if (fits) check(smgFifo.at == sRing + 3 + expected.size() && std::memcmp(sRing + 3, expected.data(), expected.size()) == 0, "a normal matrix load's bytes", 3, room);
    }
}
} // namespace

int main() {
    commands();
    runs();
    vertexTypes();
    matrices();
    std::printf("%u GX FIFO write checks passed\n", checks);
    return 0;
}
