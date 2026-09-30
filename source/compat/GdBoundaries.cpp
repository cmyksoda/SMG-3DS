// The SDK's display-list writers (GD) for programs without the whole-game library, which builds the SDK's own
// GD, and the GPU FIFO's write point. Each stops the program by name if reached.
#include <revolution/gd/GDGeometry.h>
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void unported(const char* what) {
    std::fprintf(stderr, "The Wii GPU is not the port's: %s\n", what);
    std::abort();
}
} // namespace

void GDSetArray(GXAttr, const void*, u8) { unported("GDSetArray"); }
void GDSetArrayRaw(GXAttr, u32, u8) { unported("GDSetArrayRaw"); }
void GDSetVtxDescv(const GXVtxDescList*) { unported("GDSetVtxDescv"); }
