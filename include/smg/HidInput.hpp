#pragma once
#include <smg/Input.hpp>
#include <3ds.h>

namespace smg {
// The 3DS's buttons, sticks and touch screen as the port's raw input. Motion is the caller's to add: it needs the
// gyroscope and accelerometer switched on.
inline RawInput readHidInput() {
    hidScanInput();
    const u32 held = hidKeysHeld();
    RawInput input;
    const struct { u32 native; Key port; } bindings[] = {
        {KEY_A, A}, {KEY_B, B}, {KEY_X, X}, {KEY_Y, Y},
        {KEY_L, L}, {KEY_R, R}, {KEY_ZL, ZL}, {KEY_ZR, ZR},
        {KEY_DUP, Up}, {KEY_DDOWN, Down}, {KEY_DLEFT, Left}, {KEY_DRIGHT, Right},
        {KEY_START, Start}, {KEY_SELECT, Select}
    };
    for (const auto& binding : bindings) if (held & binding.native) input.held |= binding.port;
    circlePosition circle;
    hidCircleRead(&circle);
    input.circleX = circle.dx;
    input.circleY = circle.dy;
    hidCstickRead(&circle);
    input.cstickX = circle.dx;
    input.cstickY = circle.dy;
    input.touching = (held & KEY_TOUCH) != 0;
    if (input.touching) {
        touchPosition touch;
        hidTouchRead(&touch);
        input.touchX = touch.px;
        input.touchY = touch.py;
    }
    return input;
}
} // namespace smg
