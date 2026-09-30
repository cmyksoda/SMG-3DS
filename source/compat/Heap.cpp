#include <JSystem/JKernel/JKRHeap.hpp>
#include <cstdio>
#include <cstdlib>
#include <new>

// The free store stands in for JKRHeap's current heap, so plain delete releases these as JKRHeap's own delete does.
namespace {
void* allocate(std::size_t size, int align, JKRHeap* heap, bool array) {
    // A negative alignment asks JKRHeap for the heap's tail; the free store has none, so only the size matters.
    const unsigned magnitude = align < 0 ? 0u - static_cast<unsigned>(align) : static_cast<unsigned>(align);
    if (heap || magnitude > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
        std::fprintf(stderr, "operator new: %s is not brought up\n", heap ? "allocating from a JKRHeap" : "an over-aligned allocation");
        std::abort();
    }
    return array ? ::operator new[](size) : ::operator new(size);
}
} // namespace

void* operator new(std::size_t size, int align) { return allocate(size, align, nullptr, false); }
void* operator new(std::size_t size, JKRHeap* heap, int align) { return allocate(size, align, heap, false); }
void* operator new[](std::size_t size, int align) { return allocate(size, align, nullptr, true); }
void* operator new[](std::size_t size, JKRHeap* heap, int align) { return allocate(size, align, heap, true); }
