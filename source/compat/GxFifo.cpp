// The Wii GPU's side of the whole game. The SDK's own GX writes the GPU's registers and its FIFO; the registers
// are memory GX reads back, and the FIFO's bytes, big-endian as the Wii's write-gather pipe sends them, go to a
// command processor that reads the Wii's commands as they arrive: register loads (CP, XF, BP) are kept, display
// lists are followed, and the pixel engine's draw-done and token commands raise its interrupts. Drawing is not
// written yet: the first primitive stops the program by name.
#include <revolution/os.h>
#include <revolution/os/OSInterrupt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt);

namespace {
// The processor interface, command processor, pixel engine and memory interface blocks, 4 KiB each on the Wii.
alignas(32) u8 sRegisters[4][0x1000];

// The GPU's register files as the commands leave them.
u32 sCp[0x100], sXf[0x1100], sBp[0x100];

[[noreturn]] void stop(const char* what, u32 detail) {
    std::fprintf(stderr, "The Wii GPU's command processor: %s (0x%lx)\n", what, static_cast<unsigned long>(detail));
    std::abort();
}

u32 word(const u8* at) { return u32(at[0]) << 24 | u32(at[1]) << 16 | u32(at[2]) << 8 | at[3]; }

void loadBp(u32 value) {
    const u32 address = value >> 24, data = value & 0xFFFFFF;
    sBp[address] = data;
    auto* pe = reinterpret_cast<volatile u16*>(sRegisters[2]);
    switch (address) {
    case 0x45:  // PE_DONE: the frame's commands are finished
        smgRaiseInterrupt(__OS_INTERRUPT_PI_PE_FINISH);
        break;
    case 0x47:  // PE_TOKEN: the token, readable without an interrupt
        pe[7] = static_cast<u16>(data);
        break;
    case 0x48:  // PE_TOKEN_INT: the token, with the pixel engine's token interrupt
        pe[7] = static_cast<u16>(data);
        smgRaiseInterrupt(__OS_INTERRUPT_PI_PE_TOKEN);
        break;
    }
}

// Reads one command from `at`, if all of it is there; returns its length, or 0 to wait for more bytes.
u32 command(const u8* at, u32 available);

void run(const u8* at, u32 size) {
    while (size) {
        const u32 used = command(at, size);
        if (!used) stop("a display list ends inside a command", size);
        at += used;
        size -= used;
    }
}

u32 command(const u8* at, u32 available) {
    const u8 opcode = at[0];
    if (opcode == 0x00 || opcode == 0x48) return 1;  // NOP; invalidate the vertex cache, which the port has none of
    if (opcode == 0x08) {                           // load a CP register
        if (available < 6) return 0;
        sCp[at[1]] = word(at + 2);
        return 6;
    }
    if (opcode == 0x10) {  // load XF registers: the count less one, then the first address
        if (available < 5) return 0;
        const u32 header = word(at + 1), count = (header >> 16) + 1, address = header & 0xFFFF;
        if (available < 5 + count * 4) return 0;
        for (u32 i = 0; i < count; ++i)
            if (address + i < sizeof sXf / sizeof *sXf) sXf[address + i] = word(at + 5 + i * 4);
        return 5 + count * 4;
    }
    if (opcode == 0x61) {  // load a BP register
        if (available < 5) return 0;
        loadBp(word(at + 1));
        return 5;
    }
    if (opcode == 0x40) {  // call a display list, at its physical address
        if (available < 9) return 0;
        const u32 address = word(at + 1), size = word(at + 5);
        run(static_cast<const u8*>(OSPhysicalToCached(address)), size);
        return 9;
    }
    if ((opcode & 0xE7) == 0x20) stop("indexed XF loads are not the port's yet", opcode);
    if (opcode >= 0x80 && opcode < 0xC0) stop("drawing a primitive is not the port's yet (type, vertex format)", opcode);
    stop("an opcode the Wii's GPU does not know", opcode);
}

// Bytes wait here until a whole command has come.
u8 sPending[0x10000];
u32 sPendingSize = 0;

void put(const u8* bytes, u32 count) {
    if (sPendingSize + count > sizeof sPending) stop("a command longer than the FIFO's staging", sPendingSize + count);
    std::memcpy(sPending + sPendingSize, bytes, count);
    sPendingSize += count;
    u32 read = 0;
    while (read < sPendingSize) {
        const u32 used = command(sPending + read, sPendingSize - read);
        if (!used) break;
        read += used;
    }
    std::memmove(sPending, sPending + read, sPendingSize - read);
    sPendingSize -= read;
}

void putBigEndian(u32 value, u32 bytes) {
    u8 order[4];
    for (u32 i = 0; i < bytes; ++i) order[i] = static_cast<u8>(value >> (8 * (bytes - 1 - i)));
    put(order, bytes);
}
} // namespace

extern "C" {
volatile void* smgGXRegisters(int unit) { return sRegisters[unit]; }

volatile void* smgUnportedRegisters(const char* block) {
    std::fprintf(stderr, "The Wii's %s hardware registers are not the port's\n", block);
    std::abort();
}

void smgGXFifo_u8(u8 value) { put(&value, 1); }
void smgGXFifo_s8(s8 value) { put(reinterpret_cast<const u8*>(&value), 1); }
void smgGXFifo_u16(u16 value) { putBigEndian(value, 2); }
void smgGXFifo_s16(s16 value) { putBigEndian(static_cast<u16>(value), 2); }
void smgGXFifo_u32(u32 value) { putBigEndian(value, 4); }
void smgGXFifo_s32(s32 value) { putBigEndian(static_cast<u32>(value), 4); }
void smgGXFifo_f32(f32 value) {
    u32 bits;
    std::memcpy(&bits, &value, 4);
    putBigEndian(bits, 4);
}
}
