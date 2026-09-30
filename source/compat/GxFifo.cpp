// Where the SDK's own GX writes: its register blocks are memory it reads back, and its FIFO feeds the port's command
// processor, whose pixel-engine events raise the Wii's interrupts; anything it cannot take yet stops by name.
#include <smg/GxCommands.hpp>
#include <revolution/os.h>
#include <revolution/os/OSInterrupt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt);

namespace {
// The processor interface, command processor, pixel engine and memory interface blocks, 4 KiB each on the Wii.
alignas(32) u8 sRegisters[4][0x1000];

smg::GxCommandProcessor& processor() {
    static smg::GxCommandProcessor instance({
        [](std::uint32_t physical) { return static_cast<const std::uint8_t*>(OSPhysicalToCached(physical)); },
        [] { smgRaiseInterrupt(__OS_INTERRUPT_PI_PE_FINISH); },
        [](std::uint16_t token, bool interrupt) {
            reinterpret_cast<volatile u16*>(sRegisters[2])[7] = token;  // PE_TOKEN, which GXReadDrawSync reads
            if (interrupt) smgRaiseInterrupt(__OS_INTERRUPT_PI_PE_TOKEN);
        },
        [](const char* what, std::uint32_t detail) {
            std::fprintf(stderr, "The Wii GPU's command processor: %s (0x%lx)\n", what, static_cast<unsigned long>(detail));
            std::abort();
        },
        [](std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t*) {
            std::fprintf(stderr, "The Wii GPU's command processor: drawing is not the port's yet (primitive 0x%02x, %u vertices of %lu bytes in format %u)\n",
                         kind, count, static_cast<unsigned long>(processor().vertexSize(format)), format);
            std::abort();
        },
        [](std::uint32_t command) {
            std::fprintf(stderr, "The Wii GPU's pixel engine: copying the EFB out is not the port's yet (0x%lx)\n", static_cast<unsigned long>(command));
            std::abort();
        },
    });
    return instance;
}

void putBigEndian(u32 value, u32 bytes) {
    u8 order[4];
    for (u32 i = 0; i < bytes; ++i) order[i] = static_cast<u8>(value >> (8 * (bytes - 1 - i)));
    processor().put(order, bytes);
}
} // namespace

extern "C" {
volatile void* smgGXRegisters(int unit) { return sRegisters[unit]; }

volatile void* smgUnportedRegisters(const char* block) {
    std::fprintf(stderr, "The Wii's %s hardware registers are not the port's\n", block);
    std::abort();
}

void smgGXFifo_u8(u8 value) { processor().put(&value, 1); }
void smgGXFifo_s8(s8 value) { processor().put(reinterpret_cast<const u8*>(&value), 1); }
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
