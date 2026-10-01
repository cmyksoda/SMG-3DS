// Plain new and delete for the whole game. Once the game's root heap exists, new takes from its current JKRHeap,
// as the original operator does; before that (static initialization, the port's own start-up), or inside the port's
// own work (smg::PortMemory), it takes from the free store. delete asks where the memory is: inside the Wii arenas,
// its JKRHeap frees it.
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <smg/FreeStore.hpp>
#include <smg/NativeResources.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
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

// A repacked BCSV table goes into the heap its original came from, so it lives exactly as long. The resident heaps
// are shrunk to fit once loaded (MR::adjustHeapSize), and the Wii attached tables without memory, so a table there gets
// its copy from the free store: resident data is never freed either.
void* besideInItsHeap(const void* beside, u32 size) {
    if (smgInWiiArena(beside)) {
        JKRHeap* heap = JKRHeap::findFromRoot(const_cast<void*>(beside));
        if (heap && heap->getFreeSize() >= s32(size + 0x40)) return heap->alloc(size, 4);  // room for a block header
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

// When a heap runs dry (HeapMemoryWatcher's panic), what fills it: its used blocks by size, most bytes first, and the
// first bytes of the largest, whose magic (RARC, Yaz0, J3D2, ...) names the file a block holds.
extern "C" void smgHeapCensus(void* memory) {
    const smg::PortMemory port;
    auto* heap = static_cast<JKRHeap*>(memory);
    if (heap->getHeapType() != 0x45585048) return;  // 'EXPH'
    struct Size {
        u32 size, count;
    };
    smg::FreeVector<Size> sizes;
    smg::FreeVector<const JKRExpHeap::CMemBlock*> largest;
    u32 blocks = 0, bytes = 0;
    for (const JKRExpHeap::CMemBlock* block = static_cast<JKRExpHeap*>(heap)->mHeadUsedList; block; block = block->mNext) {
        ++blocks, bytes += block->mSize;
        const auto found = std::find_if(sizes.begin(), sizes.end(), [&](const Size& s) { return s.size == block->mSize; });
        found == sizes.end() ? sizes.push_back({block->mSize, 1}) : void(++found->count);
        if (block->mSize >= 0x10000) largest.push_back(block);
    }
    std::sort(sizes.begin(), sizes.end(), [](const Size& a, const Size& b) { return u64(a.size) * a.count > u64(b.size) * b.count; });
    std::fprintf(stderr, "heap %p: %lu blocks in use, %lu bytes; by size (bytes x blocks):", memory, static_cast<unsigned long>(blocks),
                 static_cast<unsigned long>(bytes));
    for (std::size_t i = 0; i < sizes.size() && i < 24; ++i)
        std::fprintf(stderr, " %lux%lu", static_cast<unsigned long>(sizes[i].size), static_cast<unsigned long>(sizes[i].count));
    std::fputc('\n', stderr);
    std::sort(largest.begin(), largest.end(), [](auto* a, auto* b) { return a->mSize > b->mSize; });
    for (std::size_t i = 0; i < largest.size() && i < 64; ++i) {
        const auto* content = static_cast<const unsigned char*>(largest[i]->getContent());
        char text[17];
        for (int c = 0; c < 16; ++c) text[c] = std::isprint(content[c]) ? char(content[c]) : '.';
        text[16] = 0;
        // A mounted archive, already native, is named by its root folder.
        const char* name = "";
        const auto* words = reinterpret_cast<const u32*>(content);
        if (words[0] == 0x52415243 && words[2] + 0x20 <= largest[i]->mSize) {
            const auto* info = reinterpret_cast<const u32*>(content + words[2]);
            const u32 dirs = words[2] + info[1], strings = words[2] + info[5];
            if (dirs + 16 <= largest[i]->mSize && strings < largest[i]->mSize && strings + reinterpret_cast<const u32*>(content + dirs)[1] < largest[i]->mSize)
                name = reinterpret_cast<const char*>(content + strings + reinterpret_cast<const u32*>(content + dirs)[1]);
        }
        std::fprintf(stderr, "  %8lu bytes at %p group %u: %s %.40s\n", static_cast<unsigned long>(largest[i]->mSize), content, largest[i]->mGroupId,
                     text, name);
    }
}
