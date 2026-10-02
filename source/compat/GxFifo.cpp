// Where the SDK's own GX writes: its register blocks are memory it reads back, and its FIFO feeds the port's command
// processor, whose pixel-engine events raise the Wii's interrupts; anything it cannot take yet stops by name.
#include <3ds.h>
#include <smg/GxCommands.hpp>
#include <smg/GxRender.hpp>
#include <revolution/os.h>
#include <revolution/os/OSInterrupt.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt);
// GX words written so far, for the renderer's perf line (source/compat/GxRender.cpp).
extern "C" {
unsigned smgFifoWrites = 0;
}

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
        [](std::uint32_t physical, std::uint32_t size) { return smg::gxRenderDisplayList(processor(), physical, size); },
        [](bool stay) { smg::gxRenderKeepCopiesOnGpu(stay); },
    });
    return instance;
}

// Turns go in the order threads asked. The main thread blocks only on the 3DS's frame sync, inside a draw, so with
// libctru's unfair locks it relocks first every time and a set-up thread's GX writes never got in. A ticket lock in
// atomics: every GX word written takes it, so an uncontended turn costs two atomic adds; only a wait asks the kernel.
class FairLock {
public:
    void lock() {
        const u32 self = reinterpret_cast<u32>(getThreadLocalStorage());
        if (owner_ == self) return void(++depth_);
        const s32 ticket = next_.fetch_add(1, std::memory_order_relaxed);
        for (s32 serving; (serving = serving_.load(std::memory_order_acquire)) != ticket;) {
            // The kernel waits while the word is below the ticket; across the counters' wrap that test fails, so yield.
            if (serving < ticket) syncArbitrateAddress(reinterpret_cast<s32*>(&serving_), ARBITRATION_WAIT_IF_LESS_THAN, ticket);
            else svcSleepThread(0);
        }
        owner_ = self, depth_ = 1;
    }
    void unlock() {
        if (--depth_) return;
        owner_ = 0;
        const s32 next = serving_.fetch_add(1, std::memory_order_release) + 1;
        if (next_.load(std::memory_order_relaxed) != next)
            syncArbitrateAddress(reinterpret_cast<s32*>(&serving_), ARBITRATION_SIGNAL, ARBITRATION_SIGNAL_ALL);
    }

private:
    std::atomic<s32> next_{0}, serving_{0};
    volatile u32 owner_ = 0;
    unsigned depth_ = 0;
};
static_assert(sizeof(std::atomic<s32>) == sizeof(s32), "the arbiter waits on the counter's own word");

// Each thread writing GX has its own stream, so a thread preempted in the middle of a command (the Wii's single
// write-gather pipe allowed that too, in a narrower window) can't have another thread's commands spliced into it.
FairLock sFifoLock;
// Each stream's thread, by its thread-local storage pointer (a thread_local here would cost a call per word).
u32 sStreamOwners[smg::GxCommandProcessor::Streams] = {};
unsigned sStreamsGiven = 0;

void put(const u8* bytes, u32 count) {
    const u32 self = reinterpret_cast<u32>(getThreadLocalStorage());
    sFifoLock.lock();
    unsigned stream = 0;
    while (stream < sStreamsGiven && sStreamOwners[stream] != self) ++stream;
    if (stream == sStreamsGiven) {
        if (sStreamsGiven == smg::GxCommandProcessor::Streams) {
            std::fputs("The Wii GPU's FIFO: more threads write GX than the port keeps streams for\n", stderr);
            std::abort();
        }
        sStreamOwners[sStreamsGiven++] = self;
    }
    ++smgFifoWrites;
    processor().put(stream, bytes, count);
    sFifoLock.unlock();
}

// The main GX thread's writes, as the Wii's write-gather pipe gathers them: inline stores (smg/GxFifoWrite.h), handed
// to the command processor at GXFlush (which the SDK calls before every wait for draw-done or a token), before a peek,
// a movie frame or a change of how copies are kept, and when full. The Wii's GPU reads its FIFO behind the CPU in the
// same way. Other threads (rare GX writers, which may never flush) write through at once.
alignas(4) u8 sBuffer[0x4000];
} // namespace

extern "C" {
unsigned char* smgFifoAt = sBuffer;
unsigned char* smgFifoEnd = sBuffer;  // nothing gathers until the first writer is known
void* smgFifoThread = nullptr;
}

namespace {
void flushBuffer() {
    if (smgFifoAt == sBuffer) return;
    put(sBuffer, static_cast<u32>(smgFifoAt - sBuffer));
    smgFifoAt = sBuffer;
}

void flushIfBuffering() {
    if (getThreadLocalStorage() == smgFifoThread) flushBuffer();
}
} // namespace

extern "C" void (*smgGpuMemoryChanged)(const void* start, u32 size);  // source/compat/Os.cpp

namespace {
// The game's cache flushes, from any thread, some holding the CPU's lock: only noted here, never waiting on the FIFO's
// (whose holders may take the CPU's); the renderer reads the notes when it next draws.
const bool sMemoryHooked = (smgGpuMemoryChanged = &smg::gxRenderMemoryChanged, true);
} // namespace

extern "C" {
volatile void* smgGXRegisters(int unit) { return sRegisters[unit]; }

// GXPeekARGB and GXPeekZ, from any thread: the renderer is the FIFO's, so its lock is taken (after the CPU, as ever).
u32 smgPeekEfb(u16 x, u16 y, int depth) {
    flushIfBuffering();
    sFifoLock.lock();
    const u32 value = smg::gxRenderPeek(processor(), x, y, depth != 0);
    sFifoLock.unlock();
    return value;
}

// THPGXYuv2RgbDraw (source/compat/Thp.cpp): the renderer is the FIFO's, as for peeks.
void smgDrawMovieFrame(const u8* y, const u8* u, const u8* v, unsigned width, unsigned height, unsigned generation, int x, int top, int w, int h) {
    flushIfBuffering();
    sFifoLock.lock();
    smg::gxRenderMovieFrame(y, u, v, width, height, generation, x, top, w, h);
    sFifoLock.unlock();
}

volatile void* smgUnportedRegisters(const char* block) {
    std::fprintf(stderr, "The Wii's %s hardware registers are not the port's\n", block);
    std::abort();
}

void smgGXFifoFlush() { flushIfBuffering(); }

void smgGXFifoSlow(unsigned value, unsigned bytes) {
    void* const self = getThreadLocalStorage();
    if (!smgFifoThread) smgFifoThread = self, smgFifoEnd = sBuffer + sizeof sBuffer;  // the first GX writer, GXInit's: the main thread
    u8 order[4];
    for (unsigned i = 0; i < bytes; ++i) order[i] = static_cast<u8>(value >> (8 * (bytes - 1 - i)));
    if (self != smgFifoThread) return put(order, bytes);
    if (smgFifoAt + bytes > smgFifoEnd) flushBuffer();
    std::memcpy(smgFifoAt, order, bytes);
    smgFifoAt += bytes;
}
}
