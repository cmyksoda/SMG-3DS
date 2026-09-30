// The Wii Remote library's side of the port. The game reads its pads through KPAD, which the port fills from the
// 3DS (source/compat/Kpad.cpp); the Bluetooth stack behind WPAD has no counterpart. Channel 0 is the 3DS itself, a
// Remote with a Nunchuk that is always there; the other channels never connect.
#include <revolution/wpad.h>

namespace {
WPADAlloc sAlloc = nullptr;
WPADFree sFree = nullptr;
WPADConnectCallback sConnect[WPAD_MAX_CONTROLLERS] = {};
WPADExtensionCallback sExtension[WPAD_MAX_CONTROLLERS] = {};
bool sConnected = false, sExtensionReported = false;
bool validChannel(s32 channel) { return channel >= 0 && channel < WPAD_MAX_CONTROLLERS; }
} // namespace

// The Wii reports a connection from its Bluetooth stack some time after the callback is set, never inside the
// call that sets it (WPadHolder is still being built then); the port reports channel 0 at the first read.
void smgWpadReportConnections() {
    if (!sConnected && sConnect[0]) {
        sConnected = true;
        sConnect[0](0, WPAD_ERR_NONE);
    }
    if (sConnected && !sExtensionReported && sExtension[0]) {
        sExtensionReported = true;
        sExtension[0](0, WPAD_DEV_FREESTYLE);
    }
}

extern "C" {
// The work memory is the Bluetooth stack's; the native pad needs none.
u32 WPADGetWorkMemorySize() { return 0; }

// Kept as the SDK keeps them; nothing on the port's side allocates through them.
void WPADRegisterAllocator(WPADAlloc alloc, WPADFree free) {
    sAlloc = alloc;
    sFree = free;
}

// The 3DS keeps no sensor bar setting; the SDK answers below the TV when its own is missing
// (SCGetWpadSensorBarPosition). Only KPAD's pointer reads it, which the port's input stands in for.
u8 WPADGetSensorBarPosition() { return WPAD_SENSOR_BAR_POS_BOTTOM; }

WPADConnectCallback WPADSetConnectCallback(s32 channel, WPADConnectCallback callback) {
    if (!validChannel(channel)) return nullptr;
    const WPADConnectCallback previous = sConnect[channel];
    sConnect[channel] = callback;
    return previous;
}

WPADExtensionCallback WPADSetExtensionCallback(s32 channel, WPADExtensionCallback callback) {
    if (!validChannel(channel)) return nullptr;
    const WPADExtensionCallback previous = sExtension[channel];
    sExtension[channel] = callback;
    return previous;
}
}
