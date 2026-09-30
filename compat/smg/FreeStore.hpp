#pragma once
// An allocator for the port's own containers that live beside the game: straight from the free store, never from
// the game's current JKRHeap, which plain new uses once the game's heaps exist (source/compat/GameHeap.cpp).
#include <cstdlib>
#include <map>
#include <new>
#include <string>
#include <vector>

namespace smg {
template <class T> struct FreeStoreAllocator {
    using value_type = T;
    FreeStoreAllocator() = default;
    template <class U> FreeStoreAllocator(const FreeStoreAllocator<U>&) {}
    T* allocate(std::size_t count) {
        if (void* memory = std::malloc(count * sizeof(T))) return static_cast<T*>(memory);
        std::abort();
    }
    void deallocate(T* memory, std::size_t) { std::free(memory); }
    template <class U> bool operator==(const FreeStoreAllocator<U>&) const { return true; }
    template <class U> bool operator!=(const FreeStoreAllocator<U>&) const { return false; }
};
template <class T> using FreeVector = std::vector<T, FreeStoreAllocator<T>>;
using FreeString = std::basic_string<char, std::char_traits<char>, FreeStoreAllocator<char>>;
template <class K, class V> using FreeMap = std::map<K, V, std::less<K>, FreeStoreAllocator<std::pair<const K, V>>>;

// While one of these is open on a thread, plain new there takes from the free store too: port code the game calls
// into (the resource converters, the GX renderer, the DSP) keeps its scratch out of the game's heaps, where a solid
// heap would never get it back.
class PortMemory {
public:
    PortMemory();
    ~PortMemory();
    PortMemory(const PortMemory&) = delete;
    PortMemory& operator=(const PortMemory&) = delete;
};
} // namespace smg
