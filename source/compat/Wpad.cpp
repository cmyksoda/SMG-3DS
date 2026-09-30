// The Wii Remote library's side of the port. The game reads its pads through KPAD, which the port fills from the
// 3DS (source/compat/Kpad.cpp); the Bluetooth stack behind WPAD has no counterpart. Channel 0 is the 3DS itself, a
// Remote with a Nunchuk that is always there; the other channels never connect.
#include <3ds.h>
#include <revolution/wpad.h>
#include <cstdio>

namespace {
WPADAlloc sAlloc = nullptr;
WPADFree sFree = nullptr;
WPADConnectCallback sConnect[WPAD_MAX_CONTROLLERS] = {};
WPADExtensionCallback sExtension[WPAD_MAX_CONTROLLERS] = {};
bool sConnected = false, sExtensionReported = false;
WPADCallback sInfoCallback = nullptr;  // an info request answered at the next read

// The 3DS's own battery (PTMU's level 0 to 5) as a Remote's (WPAD_BATTERY_LEVEL_CRITICAL 0 to MAX 4); plugged in, it
// needs no warning.
u8 batteryLevel() {
    static const bool ready = R_SUCCEEDED(ptmuInit());
    u8 level = 5, charging = 0;
    if (ready) PTMU_GetBatteryLevel(&level), PTMU_GetBatteryChargeState(&charging);
    if (charging) return WPAD_BATTERY_LEVEL_MAX;
    return level >= 5 ? WPAD_BATTERY_LEVEL_MAX : level == 4 ? WPAD_BATTERY_LEVEL_HIGH : level == 3 ? WPAD_BATTERY_LEVEL_MEDIUM
         : level == 2 ? WPAD_BATTERY_LEVEL_LOW : WPAD_BATTERY_LEVEL_CRITICAL;
}
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
    if (const WPADCallback callback = sInfoCallback) {
        sInfoCallback = nullptr;
        callback(0, WPAD_ERR_NONE);
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

// Which device each channel holds: channel 0 the 3DS, a Remote with its Nunchuk once connected; the others none.
s32 WPADProbe(s32 channel, u32* type) {
    const bool present = channel == 0 && sConnected;
    if (type) *type = present ? WPAD_DEV_FREESTYLE : WPAD_DEV_NOT_FOUND;
    return present ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER;
}

// The 3DS is the Remote: it has no speaker of its own to switch on, and no motor. The game handles a speaker that
// won't come on (SpkSpeakerCtrl leaves it idle), and rumble simply isn't felt; each says so once in the log.
s32 WPADControlSpeaker(s32, u32, WPADCallback) {
    static bool noted = false;
    if (!noted) std::fputs("The Wii Remote's speaker sounds are not played: the 3DS has no Remote speaker\n", stderr);
    noted = true;
    return WPAD_ERR_NO_CONTROLLER;
}
BOOL WPADIsSpeakerEnabled(s32) { return FALSE; }
u8 WPADGetSpeakerVolume() { return 0; }
void WPADControlMotor(s32, u32 command) {
    static bool noted = false;
    if (command == WPAD_MOTOR_RUMBLE && !noted) std::fputs("The Wii Remote's rumble is not felt: the 3DS has no motor\n", stderr);
    noted = noted || command == WPAD_MOTOR_RUMBLE;
}

// The 3DS sleeps as the system decides, and its built-in Remote can't be disconnected.
void WPADSetAutoSleepTime(u8) {}
void WPADDisconnect(s32) {}

// Channel 0's state as its Bluetooth report would give it: the Nunchuk attached, the first LED, the 3DS's battery.
s32 WPADGetInfoAsync(s32 channel, WPADInfo* info, WPADCallback callback) {
    if (channel != 0 || !sConnected) return WPAD_ERR_NO_CONTROLLER;
    *info = {};
    info->attach = TRUE;
    info->battery = batteryLevel();
    info->lowBat = info->battery <= WPAD_BATTERY_LEVEL_LOW;
    info->nearempty = info->battery == WPAD_BATTERY_LEVEL_CRITICAL;
    info->led = 1;
    sInfoCallback = callback;
    return WPAD_ERR_NONE;
}

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
