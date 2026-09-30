// The Wii's two memory arenas, which JKRHeap carves into the game's heaps (HeapMemoryWatcher). Each is one block
// of the 3DS heap, as large as the Wii left SMG: MEM1 from the end of main.dol (0x806ADF90, rounded up past the
// OS's stacks) to 0x81800000, MEM2 from 0x90000800 to where IOS begins at 0x933E0000.
#include <revolution/os.h>
#include <revolution/os/OSAlloc.h>
#include <revolution/os/OSBootInfo.h>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr u32 sMem1Size = 0x81800000 - 0x806C0000;
constexpr u32 sMem2Size = 0x933E0000 - 0x90000800;

struct Arena {
    u8 *lo = nullptr, *hi = nullptr;  // what is left, as the SDK tracks it
    u8 *start = nullptr, *end = nullptr;  // the whole block
};

Arena& arena(Arena& which, u32 size, const char* name) {
    if (!which.lo) {
        which.lo = static_cast<u8*>(std::aligned_alloc(32, size));
        if (!which.lo) {
            std::fprintf(stderr, "The Wii's %s arena (%lu KiB) does not fit in the 3DS's memory\n", name, static_cast<unsigned long>(size / 1024));
            std::abort();
        }
        which.hi = which.lo + size;
        which.start = which.lo;
        which.end = which.hi;
    }
    return which;
}
Arena sMem1, sMem2;
Arena& mem1() { return arena(sMem1, sMem1Size, "MEM1"); }
Arena& mem2() { return arena(sMem2, sMem2Size, "MEM2"); }

// What the Wii's boot info says of the console; JKRHeap::initArena reads the memory size from it.
OSBootInfo sBootInfo = {{}, 0x0D15EA5E, 1, 0x01800000, 0, nullptr, nullptr, nullptr, 0};
} // namespace

extern "C" {
void* OSGetArenaLo() { return mem1().lo; }
void* OSGetArenaHi() { return mem1().hi; }
void OSSetArenaLo(void* lo) { mem1().lo = static_cast<u8*>(lo); }
void OSSetArenaHi(void* hi) { mem1().hi = static_cast<u8*>(hi); }
void* OSGetMEM2ArenaLo() { return mem2().lo; }
void* OSGetMEM2ArenaHi() { return mem2().hi; }
void OSSetMEM2ArenaLo(void* lo) { mem2().lo = static_cast<u8*>(lo); }
void OSSetMEM2ArenaHi(void* hi) { mem2().hi = static_cast<u8*>(hi); }

// The OS allocator keeps a descriptor per heap at the arena's start (a size and two cell lists) and hands back
// the 32-byte-aligned rest. The game only ever asks for that; its own heaps are JKRHeaps.
void* OSInitAlloc(void* arenaStart, void*, int maxHeaps) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(arenaStart) + maxHeaps * 12;
    return reinterpret_cast<void*>((start + 31) & ~uintptr_t(31));
}

OSBootInfo* smgBootInfo() { return &sBootInfo; }

// Whether memory is the game's: all its JKRHeaps are carved from these two blocks.
bool smgInWiiArena(const void* memory) {
    const u8* at = static_cast<const u8*>(memory);
    return (at >= sMem1.start && at < sMem1.end) || (at >= sMem2.start && at < sMem2.end);
}
}
