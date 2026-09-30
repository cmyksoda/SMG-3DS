// The Wii Remote library's side of the port. The game reads its pads through WPad and KPAD structures that the
// port's native pad fills (source/compat/NativePad.cpp); the Bluetooth stack behind WPAD has no counterpart.
#include <revolution/wpad.h>

namespace {
WPADAlloc sAlloc = nullptr;
WPADFree sFree = nullptr;
} // namespace

extern "C" {
// The work memory is the Bluetooth stack's; the native pad needs none.
u32 WPADGetWorkMemorySize() { return 0; }

// Kept as the SDK keeps them; nothing on the port's side allocates through them.
void WPADRegisterAllocator(WPADAlloc alloc, WPADFree free) {
    sAlloc = alloc;
    sFree = free;
}
}
