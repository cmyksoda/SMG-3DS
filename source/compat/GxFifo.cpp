// Where the SDK's own GX writes: its register blocks are memory it reads back, and its FIFO feeds the port's command
// processor, whose pixel-engine events raise the Wii's interrupts; anything it cannot take yet stops by name.
#include <3ds.h>
#include <smg/GxCommands.hpp>
#include <smg/GxRender.hpp>
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
        [](std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices) {
            smg::gxRenderPrimitive(processor(), kind, format, count, vertices);
        },
        [](std::uint32_t command) { smg::gxRenderCopy(processor(), command); },
    });
    return instance;
}

// Turns go in the order threads asked. The main thread blocks only on the 3DS's frame sync, inside a draw, so with
// libctru's unfair locks it relocks first every time and a set-up thread's GX writes never got in.
class FairLock {
public:
    void lock() {
        const u32 self = reinterpret_cast<u32>(getThreadLocalStorage());
        if (owner_ == self) return void(++depth_);
        LightLock_Lock(&guard_);
        const u32 ticket = next_++;
        while (serving_ != ticket) CondVar_Wait(&turn_, &guard_);
        owner_ = self, depth_ = 1;
        LightLock_Unlock(&guard_);
    }
    void unlock() {
        if (--depth_) return;
        LightLock_Lock(&guard_);
        owner_ = 0;
        if (next_ != ++serving_) CondVar_Broadcast(&turn_);
        LightLock_Unlock(&guard_);
    }

private:
    LightLock guard_ = 1;
    CondVar turn_ = 0;
    u32 next_ = 0, serving_ = 0;
    volatile u32 owner_ = 0;
    unsigned depth_ = 0;
};

// Each thread writing GX has its own stream, so a thread preempted in the middle of a command (the Wii's single
// write-gather pipe allowed that too, in a narrower window) can't have another thread's commands spliced into it.
FairLock sFifoLock;
unsigned sStreamsGiven = 0;

void put(const u8* bytes, u32 count) {
    thread_local int stream = -1;
    sFifoLock.lock();
    if (stream < 0) {
        if (sStreamsGiven == smg::GxCommandProcessor::Streams) {
            std::fputs("The Wii GPU's FIFO: more threads write GX than the port keeps streams for\n", stderr);
            std::abort();
        }
        stream = static_cast<int>(sStreamsGiven++);
    }
    processor().put(static_cast<unsigned>(stream), bytes, count);
    sFifoLock.unlock();
}

void putBigEndian(u32 value, u32 bytes) {
    u8 order[4];
    for (u32 i = 0; i < bytes; ++i) order[i] = static_cast<u8>(value >> (8 * (bytes - 1 - i)));
    put(order, bytes);
}
} // namespace

extern "C" {
volatile void* smgGXRegisters(int unit) { return sRegisters[unit]; }

// GXPeekARGB and GXPeekZ, from any thread: the renderer is the FIFO's, so its lock is taken (after the CPU, as ever).
u32 smgPeekEfb(u16 x, u16 y, int depth) {
    sFifoLock.lock();
    const u32 value = smg::gxRenderPeek(processor(), x, y, depth != 0);
    sFifoLock.unlock();
    return value;
}

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
