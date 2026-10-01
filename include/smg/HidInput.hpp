#pragma once
#include <smg/Input.hpp>
#include <3ds.h>

namespace smg {
// The 3DS's buttons, sticks and touch screen as the port's raw input; HidMotion adds the motion sensors.
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

// The gyroscope and accelerometer, for the pointer's motion mode; `ready` is false where they didn't start.
class HidMotion {
public:
    bool start() {
        float rawPerDps = 0;
        ready_ = R_SUCCEEDED(HIDUSER_EnableAccelerometer()) && R_SUCCEEDED(HIDUSER_EnableGyroscope()) &&
                 R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&rawPerDps)) && rawPerDps > 0;
        dpsPerRaw_ = ready_ ? 1.0f / rawPerDps : 0;
        return ready_;
    }
    void stop() {
        HIDUSER_DisableGyroscope();
        HIDUSER_DisableAccelerometer();
        ready_ = false;
    }
    bool ready() const { return ready_; }
    void read(RawInput& input) const {
        if (!ready_) return;
        angularRate rate;
        accelVector accel;
        hidGyroRead(&rate);
        hidAccelRead(&accel);
        // libctru declares the ring entry as {x, z, y}; keep the ring's own order.
        input.gyro[0] = rate.x * dpsPerRaw_;
        input.gyro[1] = rate.z * dpsPerRaw_;
        input.gyro[2] = rate.y * dpsPerRaw_;
        input.accel[0] = accel.x;
        input.accel[1] = accel.y;
        input.accel[2] = accel.z;
        input.motion = true;
    }

private:
    bool ready_ = false;
    float dpsPerRaw_ = 0;
};
} // namespace smg
