#pragma once

#include <Game/Util/TriggerChecker.hpp>
#include <smg/Motion.hpp>
#include <smg/Settings.hpp>
#include <cstdint>

namespace smg {
struct Rect {
    int x, y, width, height;
    constexpr bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};
// Bottom-screen touch targets. The corner button on the right shoots in stylus mode
// and recenters the pointer otherwise.
inline constexpr Rect AimArea{0, 0, 320, 240};
inline constexpr Rect CameraButton{8, 190, 48, 44};
inline constexpr Rect ShootButton{264, 190, 48, 44};
inline constexpr Rect RecenterButton = ShootButton;
inline constexpr Rect OptionsButton{252, 4, 64, 24};
inline constexpr float TopWidth = 400, TopHeight = 240;

enum class Option : std::uint8_t {
    PointerMotion, PointerCStick, PointerStylus, MotionSlower, MotionFaster,
    StickSlower, StickFaster, InvertX, InvertY, ResetButtons, Remap, Done, Count
};
struct OptionButton { Option option; Rect bounds; };
inline constexpr OptionButton OptionButtons[]{
    {Option::PointerMotion, {88, 36, 72, 30}}, {Option::PointerCStick, {164, 36, 72, 30}},
    {Option::PointerStylus, {240, 36, 72, 30}},
    {Option::MotionSlower, {88, 74, 40, 30}}, {Option::MotionFaster, {272, 74, 40, 30}},
    {Option::StickSlower, {88, 112, 40, 30}}, {Option::StickFaster, {272, 112, 40, 30}},
    {Option::InvertX, {88, 150, 110, 30}}, {Option::InvertY, {202, 150, 110, 30}},
    {Option::ResetButtons, {88, 196, 110, 30}}, {Option::Remap, {202, 196, 110, 30}}, {Option::Done, OptionsButton},
};
// One row per action on the remap page, below its title.
constexpr Rect remapRow(unsigned action) { return {8, 34 + static_cast<int>(action) * 24, 304, 22}; }
Settings applyOption(Option option, Settings settings);
bool optionActive(Option option, const Settings& settings);

struct RawInput {
    std::uint32_t held = 0;
    int circleX = 0, circleY = 0;
    int cstickX = 0, cstickY = 0;
    bool touching = false;
    int touchX = 0, touchY = 0;
    bool motion = false;
    float gyro[3]{}, accel[3]{}; // HID order: pitch, yaw, roll in deg/s; accel in any unit
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
    float cameraX = 0.0f, cameraY = 0.0f; // C-stick, unless it drives the pointer
    Button jump, spin, crouch, shoot, camera, recenter, pause, minus;
    Button dpadUp, dpadDown, dpadLeft, dpadRight;
    Pointer pointer;
    PointerMode pointerMode = PointerMode::Motion;
    bool shootMode = false; // stylus mode's touch-to-shoot toggle
    bool shootButtonHeld = false, cameraButtonHeld = false, recenterButtonHeld = false;
    bool optionsOpen = false, optionsButtonHeld = false;
    int heldOption = -1;
    bool remapOpen = false;
    int capturing = -1; // the action whose buttons are being chosen
};
inline float touchButtonOpacity(const InputFrame& frame) {
    return frame.pointerMode == PointerMode::Stylus && frame.pointer.visible ? 0.5f : 1.0f;
}

class Input {
public:
    // Without touchControls (a screen that doesn't draw them), a touch only aims the stylus pointer.
    explicit Input(const Settings& settings = {}, bool touchControls = true);
    const InputFrame& update(const RawInput& raw, float seconds = 1.0f / 60.0f);
    void cancelGesture();
    const InputFrame& frame() const { return frame_; }
    const Settings& settings() const { return settings_; }
    void setSettings(const Settings& settings);
    // True once after an options-page change, so the caller can save it.
    bool takeSettingsChanged();
private:
    enum class Gesture { None, Aim, Camera, Shoot, Recenter, OptionsTab, Option, RemapRow, Ignored };
    static Button sample(TriggerChecker& checker, bool held);
    void beginGesture(const RawInput& raw);
    void recenter();
    void setPointer(float x, float y);
    Settings settings_;
    MotionTracker motion_;
    Gesture gesture_ = Gesture::None;
    int gestureOption_ = -1;
    bool touchControls_ = true, touching_ = false, suppressUntilLift_ = false, shootMode_ = false;
    bool optionsOpen_ = false, settingsChanged_ = false, remapOpen_ = false, captureFresh_ = false;
    int capturing_ = -1;
    std::uint32_t previousHeld_ = 0, suppressed_ = 0;
    float pointerX_ = (TopWidth - 1) / 2, pointerY_ = (TopHeight - 1) / 2;
    TriggerChecker actions_[ActionCount];
    TriggerChecker shoot_, camera_, recenter_;
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
