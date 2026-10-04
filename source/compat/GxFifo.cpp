// Where the SDK's own GX writes: its register blocks are memory it reads back, and its FIFO feeds the port's command
// processor, whose pixel-engine events raise the Wii's interrupts; anything it cannot take yet stops by name.
#include <3ds.h>
#include <smg/FreeStore.hpp>
#include <smg/GxCommands.hpp>
#include <smg/GxFifoWrite.h>
#include <smg/GxRender.hpp>
#include <smg/GxZones.hpp>
#include <revolution/os.h>
#include <revolution/os/OSInterrupt.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt);
extern "C" void smgDrawSyncRead();
extern "C" void smgDisplayCopyRead();
extern "C" int smgDevGxInline();  // source/compat/Kpad.cpp
extern "C" int smgDevGxTiming();
extern "C" void smgMarkGameThread();  // source/compat/Os.cpp
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
            if (interrupt) smgRaiseInterrupt(__OS_INTERRUPT_PI_PE_TOKEN), smgDrawSyncRead();
        },
        [](const char* what, std::uint32_t detail) {
            std::fprintf(stderr, "The Wii GPU's command processor: %s (0x%lx)\n", what, static_cast<unsigned long>(detail));
            std::abort();
        },
        [](std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices) {
            smg::gxRenderPrimitive(processor(), kind, format, count, vertices);
        },
        [](std::uint32_t command) {
            smg::gxRenderCopy(processor(), command);
            if (command >> 14 & 1) smgDisplayCopyRead();
        },
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

// The processor and the renderer are one thread's at a time: the render thread's, or (inline, and for peeks and
// movie frames once the render thread is idle) the caller's.
FairLock sFifoLock;
smg::GxThreadClock sClock;

// Each thread writing GX has its own stream, so a thread preempted in the middle of a command (the Wii's single
// write-gather pipe allowed that too, in a narrower window) can't have another thread's commands spliced into it.
LightLock sStreamLock = 1;
u32 sStreamOwners[smg::GxCommandProcessor::Streams] = {};
unsigned sStreamsGiven = 0;

unsigned streamOf(u32 self) {
    LightLock_Lock(&sStreamLock);
    unsigned stream = 0;
    while (stream < sStreamsGiven && sStreamOwners[stream] != self) ++stream;
    if (stream == sStreamsGiven) {
        if (sStreamsGiven == smg::GxCommandProcessor::Streams) {
            std::fputs("The Wii GPU's FIFO: more threads write GX than the port keeps streams for\n", stderr);
            std::abort();
        }
        sStreamOwners[sStreamsGiven++] = self;
    }
    LightLock_Unlock(&sStreamLock);
    return stream;
}

// `zoned`: the render thread's own reading, marked in its zone (smg/GxZones.hpp).
void process(unsigned stream, const u8* bytes, u32 count, bool zoned = false) {
    const u64 start = svcGetSystemTick();
    if (zoned) smg::gxZone = smg::ZoneLock;
    sFifoLock.lock();
    if (zoned) smg::gxZone = smg::ZoneParse;
    ++smgFifoWrites;
    processor().put(stream, bytes, count);
    sFifoLock.unlock();
    if (zoned) smg::gxZone = smg::ZoneIdle;
    sClock.busy.fetch_add(static_cast<u32>(svcGetSystemTick() - start), std::memory_order_relaxed);
    sClock.bytes.fetch_add(count, std::memory_order_relaxed);
}

// The main GX thread's writes, as the Wii's write-gather pipe gathers them: inline stores (smg/GxFifoWrite.h). Inline,
// they gather here and are read at GXFlush (which the SDK calls before every wait for draw-done or a token), before a
// peek, a movie frame or a change of how copies are kept, and when full. Other threads write through at once.
alignas(4) u8 sBuffer[0x4000];

// The render thread: the stream is read and drawn on another core behind the game, as the Wii's GPU reads its FIFO
// behind the CPU. The game waits for it only where Wii games wait for their GPU (draw done, tokens) and where the port
// reads the GPU's results (peeks, movie frames); its interrupts are raised from there, as the audio thread raises its own.
// The game thread's writes go into a ring, handed over a piece at a time: at GXFlush, and every Chunk bytes.
constexpr u32 RingBytes = 0x80000, Chunk = 0x4000;
alignas(4) u8 sRing[RingBytes];
struct Entry {
    const u8* bytes;     // in the ring, or null for `word`: another thread's single write
    u32 size, ringBytes;  // bytes to read, and ring bytes freed once read (a skip to the ring's start frees its tail)
    u8 stream, word[4];
};
constexpr unsigned MaxEntries = 1024;
Entry sEntries[MaxEntries];
LightLock sQueueLock = 1;
CondVar sWork = 0, sDone = 0;
unsigned sHead = 0, sTail = 0;  // entries [sTail, sHead), under sQueueLock
bool sBusy = false;
u32 sProduced = 0, sConsumed = 0;  // ring bytes as running totals: the game thread's, and the render thread's (under the lock)
Thread sRenderThread = nullptr;
void* sRenderTls = nullptr;
u8* sPublishFrom = sRing;  // the game thread's first byte not handed over yet
unsigned sMainStream = 0;

void renderLoop(void*) {
    sRenderTls = getThreadLocalStorage();
    for (;;) {
        LightLock_Lock(&sQueueLock);
        while (sTail == sHead) {
            sBusy = false;
            CondVar_Broadcast(&sDone);
            CondVar_Wait(&sWork, &sQueueLock);
        }
        const Entry entry = sEntries[sTail % MaxEntries];
        sBusy = true;
        LightLock_Unlock(&sQueueLock);
        if (entry.size) process(entry.stream, entry.bytes ? entry.bytes : entry.word, entry.size, true);
        LightLock_Lock(&sQueueLock);
        ++sTail, sConsumed += entry.ringBytes;
        CondVar_Broadcast(&sDone);
        LightLock_Unlock(&sQueueLock);
    }
}

// The game waiting on the render thread (for room, or for it to finish): timed for the perf line.
template <class Done> void waitFor(Done done) {
    const u64 start = svcGetSystemTick();
    LightLock_Lock(&sQueueLock);
    while (!done()) CondVar_Wait(&sDone, &sQueueLock);
    LightLock_Unlock(&sQueueLock);
    sClock.gameWaits.fetch_add(static_cast<u32>(svcGetSystemTick() - start), std::memory_order_relaxed);
}

void enqueue(const Entry& entry) {
    for (;;) {
        LightLock_Lock(&sQueueLock);
        if (sHead - sTail < MaxEntries) break;
        LightLock_Unlock(&sQueueLock);
        waitFor([] { return sHead - sTail < MaxEntries; });
    }
    sEntries[sHead++ % MaxEntries] = entry;
    CondVar_Signal(&sWork);
    LightLock_Unlock(&sQueueLock);
}

// What the game thread wrote since it last handed over.
void publish() {
    const u32 size = static_cast<u32>(smgFifoAt - sPublishFrom);
    if (!size) return;
    sProduced += size;
    enqueue({sPublishFrom, size, size, static_cast<u8>(sMainStream), {}});
    sPublishFrom = smgFifoAt;
}

// Room for `bytes` more at the game thread's write point, in one piece: a skip to the ring's start when its end is
// near, a wait while the render thread is a whole ring behind.
void makeRoom(u32 bytes) {
    publish();
    for (;;) {
        LightLock_Lock(&sQueueLock);
        const u32 consumed = sConsumed;
        LightLock_Unlock(&sQueueLock);
        const u32 free = RingBytes - (sProduced - consumed), toEnd = RingBytes - static_cast<u32>(smgFifoAt - sRing);
        if (toEnd < bytes) {
            if (free < toEnd) {
                waitFor([consumed] { return sConsumed != consumed; });
                continue;
            }
            sProduced += toEnd;
            enqueue({nullptr, 0, toEnd, 0, {}});
            smgFifoAt = sPublishFrom = sRing;
            continue;
        }
        if (free >= bytes) {
            smgFifoEnd = smgFifoAt + std::min({free, toEnd, Chunk});
            return;
        }
        waitFor([consumed] { return sConsumed != consumed; });
    }
}

// Everything handed over so far read and drawn, before another thread reads what the GPU made.
void drain() {
    if (!sRenderThread || getThreadLocalStorage() == sRenderTls) return;
    waitFor([] { return sHead == sTail && !sBusy; });
}

// The New 3DS's third core, which the game's own threads (all on the app core, by priority) never use; else the
// system core at the app's largest share of it; else none, and the stream is drawn on the game's thread as it flushes.
void startRenderThread() {
    if (smgDevGxInline()) return void(std::fputs("The Wii GPU's FIFO: drawn on the game's thread (dev.gx_inline)\n", stderr));
    constexpr size_t Stack = 0x20000;
    sRenderThread = threadCreate(renderLoop, nullptr, Stack, 0x18, 2, true);
    sClock.core = 2;
    if (!sRenderThread && R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) sRenderThread = threadCreate(renderLoop, nullptr, Stack, 0x18, 1, true), sClock.core = 1;
    if (!sRenderThread) sClock.core = -1;
    std::fprintf(stderr, "The Wii GPU's FIFO: drawn %s\n", sRenderThread ? sClock.core == 2 ? "on core 2" : "on core 1 (80%)" : "on the game's thread (no core free)");
}

void begin(void* self) {
    processor().countCommands = smgDevGxTiming();
    smgFifoThread = self;
    smgMarkGameThread();
    sMainStream = streamOf(reinterpret_cast<u32>(self));
    startRenderThread();
    if (sRenderThread) smgFifoAt = sPublishFrom = sRing, smgFifoEnd = sRing + Chunk;
    else smgFifoAt = sBuffer, smgFifoEnd = sBuffer + sizeof sBuffer;
}

void flushBuffer() {
    if (sRenderThread) return publish();
    if (smgFifoAt == sBuffer) return;
    process(sMainStream, sBuffer, static_cast<u32>(smgFifoAt - sBuffer));
    smgFifoAt = sBuffer;
}

void flushIfBuffering() {
    if (getThreadLocalStorage() == smgFifoThread) flushBuffer();
}

// Another thread's write: after what the game thread has handed over, as before its own buffered words.
void writeThrough(void* self, const u8* bytes, unsigned count) {
    const unsigned stream = streamOf(reinterpret_cast<u32>(self));
    if (!sRenderThread || self == sRenderTls) return process(stream, bytes, count);
    Entry entry{nullptr, count, 0, static_cast<u8>(stream), {}};
    std::memcpy(entry.word, bytes, count);
    enqueue(entry);
}
} // namespace

extern "C" {
unsigned char* smgFifoAt = sBuffer;
unsigned char* smgFifoEnd = sBuffer;  // nothing gathers until the first writer is known
void* smgFifoThread = nullptr;
}

extern "C" void (*smgGpuMemoryChanged)(const void* start, u32 size);  // source/compat/Os.cpp

namespace {
// The game's cache flushes, from any thread, some holding the CPU's lock: only noted here, never waiting on the FIFO's
// (whose holders may take the CPU's); the renderer reads the notes when it next draws.
const bool sMemoryHooked = (smgGpuMemoryChanged = &smg::gxRenderMemoryChanged, true);
} // namespace

namespace smg {
GxThreadClock& gxThreadClock() { return sClock; }
} // namespace smg

extern "C" {
volatile void* smgGXRegisters(int unit) { return sRegisters[unit]; }

// GXPeekARGB and GXPeekZ, from any thread: after everything written before them is drawn. From the render thread (a
// draw-sync callback run as its token is read) the renderer is already this thread's.
u32 smgPeekEfb(u16 x, u16 y, int depth) {
    flushIfBuffering();
    drain();
    sFifoLock.lock();
    const u32 value = smg::gxRenderPeek(processor(), x, y, depth != 0);
    sFifoLock.unlock();
    return value;
}

// THPGXYuv2RgbDraw (source/compat/Thp.cpp): drawn in order after what was written before it, as for peeks.
void smgDrawMovieFrame(const u8* y, const u8* u, const u8* v, unsigned width, unsigned height, unsigned generation, int x, int top, int w, int h) {
    flushIfBuffering();
    drain();
    sFifoLock.lock();
    smg::gxRenderMovieFrame(y, u, v, width, height, generation, x, top, w, h);
    sFifoLock.unlock();
}

volatile void* smgUnportedRegisters(const char* block) {
    std::fprintf(stderr, "The Wii's %s hardware registers are not the port's\n", block);
    std::abort();
}

void smgGXFifoFlush() { flushIfBuffering(); }

// The game's own thread: the one that writes GX (GXInit's), for memos only it keeps.
int smgOnGameThread() { return getThreadLocalStorage() == smgFifoThread; }

void smgGXFifoBytes(const void* bytes, unsigned size) {
    const u8* from = static_cast<const u8*>(bytes);
    if (getThreadLocalStorage() != smgFifoThread || !sRenderThread) {
        for (unsigned i = 0; i < size; ++i) smgGXFifoSlow(from[i], 1);
        return;
    }
    while (size) {
        if (smgFifoAt == smgFifoEnd) makeRoom(1);
        const unsigned n = std::min<unsigned>(size, static_cast<unsigned>(smgFifoEnd - smgFifoAt));
        std::memcpy(smgFifoAt, from, n);
        smgFifoAt += n, from += n, size -= n;
    }
}

// J3D's draw matrices are double-buffered by calcAnim and read by index (GX_LOAD_INDX_A-D) from arrays 12-15. The
// render thread reads a frame behind the game, so a load names the data the game sees as it sends it.
namespace {
const u8* sMatrixArrays[4] = {};
u32 sMatrixStrides[4] = {};
}

void smgGXMatrixArray(unsigned array, const void* base, unsigned stride) {
    if (array < 4) sMatrixArrays[array] = static_cast<const u8*>(base), sMatrixStrides[array] = stride;
}

void smgGXLoadIndexedNow(unsigned command, unsigned index, unsigned target) {
    const unsigned array = command >> 3 & 3, count = (target >> 12 & 0xF) + 1, address = target & 0xFFF;
    if (!sMatrixArrays[array]) {  // an array set some other way: the GPU reads it, as it did
        smgGXFifo_u8(static_cast<unsigned char>(command)), smgGXFifo_u16(static_cast<unsigned short>(index));
        return smgGXFifo_u16(static_cast<unsigned short>(target));
    }
    const u8* from = sMatrixArrays[array] + index * sMatrixStrides[array];
    const unsigned bytes = 5 + 4 * count;
    if (getThreadLocalStorage() == smgFifoThread && sRenderThread) {
        // The usual case, the game thread's: the whole load stored at once, each word big-endian as the stream holds it.
        if (smgFifoAt + bytes > smgFifoEnd) makeRoom(bytes);
        u8* at = smgFifoAt;
        at[0] = 0x10, at[1] = 0, at[2] = static_cast<u8>(count - 1), at[3] = static_cast<u8>(address >> 8), at[4] = static_cast<u8>(address);
        for (unsigned i = 0; i < count; ++i) {
            u32 word;
            std::memcpy(&word, from + 4 * i, 4);  // the CPU's own order, as the processor reads an array
            word = __builtin_bswap32(word);
            std::memcpy(at + 5 + 4 * i, &word, 4);
        }
        smgFifoAt = at + bytes;
        return;
    }
    smgGXFifo_u8(0x10), smgGXFifo_u16(static_cast<unsigned short>(count - 1)), smgGXFifo_u16(static_cast<unsigned short>(address));
    for (unsigned i = 0; i < count; ++i) {
        u32 word;
        std::memcpy(&word, from + 4 * i, 4);
        smgGXFifo_u32(word);
    }
}

// Draw syncs the game set and those the render thread has read: before a scene is destroyed the game waits for them
// all, then takes any of their interrupts still waiting for the CPU, so no callback reads a scene that is gone (as the
// Wii's had all run by then). Handlers are not waited on: one may wait for a message queue itself.
namespace {
std::atomic<s32> sDrawSyncsIssued{0}, sDrawSyncsRead{0};
}
extern "C" void smgDrawSyncIssued() { sDrawSyncsIssued.fetch_add(1, std::memory_order_relaxed); }
void smgDrawSyncRead() {
    sDrawSyncsRead.fetch_add(1, std::memory_order_release);
    syncArbitrateAddress(reinterpret_cast<s32*>(&sDrawSyncsRead), ARBITRATION_SIGNAL, -1);
}
extern "C" void smgWaitDrawSyncs() {
    const s32 issued = sDrawSyncsIssued.load(std::memory_order_relaxed);
    flushIfBuffering();
    while (sDrawSyncsRead.load(std::memory_order_acquire) - issued < 0)
        syncArbitrateAddress(reinterpret_cast<s32*>(&sDrawSyncsRead), ARBITRATION_WAIT_IF_LESS_THAN, issued);
    OSRestoreInterrupts(OSDisableInterrupts());
}

// A drawn step waits only for the frames before it, so a frame draws while the game runs the next two steps: what those
// rewrite goes into the FIFO when sent (J3D's matrices and lists) or is double-buffered by drawn frame.
namespace {
std::atomic<s32> sCopiesIssued{0}, sCopiesRead{0};
u32 sDrawnFrames = 0;
LightLock sStampLock = 1;
// From the free store: plain new would take the scene's heap, gone at the next scene while the map kept its nodes.
std::unordered_map<const void*, u32, std::hash<const void*>, std::equal_to<const void*>, smg::FreeStoreAllocator<std::pair<const void* const, u32>>> sStamps;
}
void smgDisplayCopyIssued() { sCopiesIssued.fetch_add(1, std::memory_order_relaxed); }
void smgDisplayCopyRead() {
    sCopiesRead.fetch_add(1, std::memory_order_release);
    syncArbitrateAddress(reinterpret_cast<s32*>(&sCopiesRead), ARBITRATION_SIGNAL, -1);
}
void smgWaitFramesOut() {
    const s32 issued = sCopiesIssued.load(std::memory_order_relaxed);
    flushIfBuffering();
    const u64 start = svcGetSystemTick();
    while (sCopiesRead.load(std::memory_order_acquire) - issued < 0)
        syncArbitrateAddress(reinterpret_cast<s32*>(&sCopiesRead), ARBITRATION_WAIT_IF_LESS_THAN, issued);
    sClock.gameWaits.fetch_add(static_cast<u32>(svcGetSystemTick() - start), std::memory_order_relaxed);
}
void smgFrameDrawn() { ++sDrawnFrames; }
// A double buffer the game swaps at every write (J3D's deformed vertices): swapped only when a frame has been drawn
// since its owner last wrote, so the buffer a frame in flight reads is never the one written.
int smgSwapSinceDrawn(const void* owner) {
    LightLock_Lock(&sStampLock);
    u32& stamp = sStamps.try_emplace(owner, sDrawnFrames - 1).first->second;
    const bool swap = stamp != sDrawnFrames;
    stamp = sDrawnFrames;
    LightLock_Unlock(&sStampLock);
    return swap;
}

void smgGXFifoSlow(unsigned value, unsigned bytes) {
    void* const self = getThreadLocalStorage();
    if (!smgFifoThread) begin(self);  // the first GX writer, GXInit's: the main thread
    u8 order[4];
    for (unsigned i = 0; i < bytes; ++i) order[i] = static_cast<u8>(value >> (8 * (bytes - 1 - i)));
    if (self != smgFifoThread) return writeThrough(self, order, bytes);
    if (smgFifoAt + bytes > smgFifoEnd) {
        if (sRenderThread) makeRoom(bytes);
        else flushBuffer();
    }
    std::memcpy(smgFifoAt, order, bytes);
    smgFifoAt += bytes;
}
}
