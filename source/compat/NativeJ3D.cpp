// J3D's files turned native where JSystem's loaders first meet them (compat/smg/NativeResources.hpp); built with the
// J3D library, beside those loaders.
#include <revolution/types.h>
#include <smg/FreeStore.hpp>
#include <smg/J3D.hpp>
#include <smg/NativeResources.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void smgNativeJ3DModel(void* file) {
    auto* bytes = static_cast<std::uint8_t*>(file);
    if (std::memcmp(bytes, "J3D2", 4) != 0) return;  // already native (its magic reads as a word now), or not J3D2
    const smg::PortMemory memory;
    const u32 size = u32(bytes[8]) << 24 | u32(bytes[9]) << 16 | u32(bytes[10]) << 8 | bytes[11];
    if (const char* why = smg::nativeJ3DModel(bytes, size)) {
        std::fprintf(stderr, "A J3D model (%.4s) %s\n", reinterpret_cast<const char*>(bytes + 4), why);
        std::abort();
    }
}

void smgNativeJ3DAnimation(void* file) {
    auto* bytes = static_cast<std::uint8_t*>(file);
    if (std::memcmp(bytes, "J3D1", 4) != 0) return;  // already native, or not J3D1
    const smg::PortMemory memory;
    const u32 size = u32(bytes[8]) << 24 | u32(bytes[9]) << 16 | u32(bytes[10]) << 8 | bytes[11];
    if (const char* why = smg::nativeJ3DAnimation(bytes, size)) {
        std::fprintf(stderr, "A J3D animation (%.4s) %s\n", reinterpret_cast<const char*>(bytes + 4), why);
        std::abort();
    }
}
