#pragma once

#include <Game/Util/TriggerChecker.hpp>
#include <cstdint>

namespace smg {
enum Key : std::uint32_t {
    A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3,
    L = 1u << 4, R = 1u << 5, Up = 1u << 6, Down = 1u << 7,
    Left = 1u << 8, Right = 1u << 9, Start = 1u << 10, Select = 1u << 11
};
struct Rect {
    int x, y, width, height;
    constexpr bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};
inline constexpr Rect AimArea{0, 0, 320, 240};
inline constexpr Rect CameraButton{8, 190, 48, 44};
inline constexpr Rect ShootButton{264, 190, 48, 44};

struct RawInput {
    std::uint32_t held = 0;
    int circleX = 0, circleY = 0;
    bool touching = false;
    int touchX = 0, touchY = 0;
};
struct Button {
    bool held = false, pressed = false, released = false;
};
struct Pointer {
    bool visible = false;
    float x = 0.0f, y = 0.0f;
    float ndcX = -1.0f, ndcY = 1.0f;
};
struct InputFrame {
    float moveX = 0.0f, moveY = 0.0f;
    Button jump, spin, crouch, shoot, camera, pause, select;
    Button dpadUp, dpadDown, dpadLeft, dpadRight;
    Pointer pointer;
    bool shootMode = false;
    bool shootButtonHeld = false, cameraButtonHeld = false;
};
inline float touchButtonOpacity(const InputFrame& frame) {
    return frame.pointer.visible ? 0.5f : 1.0f;
}

class Input {
public:
    const InputFrame& update(const RawInput& raw);
    void cancelGesture();
    const InputFrame& frame() const { return frame_; }
private:
    enum class Gesture { None, Aim, Camera, Shoot, Ignored };
    static Button sample(TriggerChecker& checker, bool held);
    Gesture gesture_ = Gesture::None;
    bool touching_ = false, suppressUntilLift_ = false, shootMode_ = false;
    TriggerChecker jump_, spin_, crouch_, shoot_, camera_, pause_, select_;
    TriggerChecker up_, down_, left_, right_;
    InputFrame frame_;
};

// Wii button values used by Petari's WPadButton and GamePadUtil.
enum WiiButton : std::uint32_t {
    WiiLeft = 0x0001, WiiRight = 0x0002, WiiDown = 0x0004, WiiUp = 0x0008,
    WiiPlus = 0x0010, WiiB = 0x0400, WiiA = 0x0800, WiiMinus = 0x1000,
    WiiZ = 0x2000, WiiC = 0x4000
};
struct WiiInput {
    std::uint32_t held = 0, pressed = 0, released = 0;
    float stickX = 0, stickY = 0;
    bool swing = false, swingTrigger = false;
    Pointer pointer;
};
WiiInput toWiiInput(const InputFrame& frame);
} // namespace smg
