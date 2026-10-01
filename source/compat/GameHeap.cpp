// Plain new and delete for the whole game. Once the game's root heap exists, new takes from its current JKRHeap,
// as the original operator does; before that (static initialization, the port's own start-up), or inside the port's
// own work (smg::PortMemory), it takes from the free store. delete asks where the memory is: inside the Wii arenas,
// its JKRHeap frees it.
#include <JSystem/JKernel/JKRHeap.hpp>
#include <smg/FreeStore.hpp>
#include <smg/NativeResources.hpp>
#include <cstdlib>
#include <new>

extern "C" bool smgInWiiArena(const void* memory);

namespace {
void* allocate(std::size_t size) {
    if (JKRHeap::sCurrentHeap && !smg::PortMemory::active()) return JKRHeap::alloc(size, 4, nullptr);
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    std::abort();
}

void release(void* memory) {
    if (!memory) return;
    if (smgInWiiArena(memory)) JKRHeap::free(memory, nullptr);
    else std::free(memory);
}

// A repacked BCSV table goes into the heap its original came from, so it lives exactly as long.
void* besideInItsHeap(const void* beside, u32 size) {
    if (smgInWiiArena(beside)) {
        if (JKRHeap* heap = JKRHeap::findFromRoot(const_cast<void*>(beside))) return heap->alloc(size, 4);
    }
    return std::malloc(size);
}
const bool sBcsvAllocatorSet = (smgBcsvAllocator = besideInItsHeap, true);
} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* memory) noexcept { release(memory); }
void operator delete[](void* memory) noexcept { release(memory); }
void operator delete(void* memory, std::size_t) noexcept { release(memory); }
void operator delete[](void* memory, std::size_t) noexcept { release(memory); }
