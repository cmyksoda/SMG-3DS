#include <smg/Input.hpp>
#include <algorithm>
#include <cmath>

namespace smg {
namespace {
// A 36-degree turn crosses the top screen at 100% speed.
constexpr float PixelsPerDegree = TopWidth / 36.0f;
constexpr float StickPixelsPerSecond = 420.0f;

void radial(int rawX, int rawY, float deadzone, float fullTravel, float& outX, float& outY) {
    const float x = static_cast<float>(rawX), y = static_cast<float>(rawY);
    const float radius = std::sqrt(x * x + y * y);
    outX = outY = 0;
    if (radius <= deadzone) return;
    const float magnitude = std::min(1.0f, (radius - deadzone) / (fullTravel - deadzone));
    outX = x / radius * magnitude;
    outY = y / radius * magnitude;
}

int stepSpeed(int speed, int direction) {
    return std::clamp(speed + direction * Settings::SpeedStep, Settings::MinSpeed, Settings::MaxSpeed);
}
} // namespace

Settings applyOption(Option option, Settings settings) {
    switch (option) {
    case Option::PointerMotion: settings.pointer = PointerMode::Motion; break;
    case Option::PointerCStick: settings.pointer = PointerMode::CStick; break;
    case Option::PointerStylus: settings.pointer = PointerMode::Stylus; break;
    case Option::MotionSlower: settings.motionSpeed = stepSpeed(settings.motionSpeed, -1); break;
    case Option::MotionFaster: settings.motionSpeed = stepSpeed(settings.motionSpeed, 1); break;
    case Option::StickSlower: settings.stickSpeed = stepSpeed(settings.stickSpeed, -1); break;
    case Option::StickFaster: settings.stickSpeed = stepSpeed(settings.stickSpeed, 1); break;
    case Option::InvertX: settings.invertX = !settings.invertX; break;
    case Option::InvertY: settings.invertY = !settings.invertY; break;
    case Option::ResetButtons: settings.bindings = defaultBindings(); break;
    default: break;
    }
    return settings;
}

bool optionActive(Option option, const Settings& settings) {
    switch (option) {
    case Option::PointerMotion: return settings.pointer == PointerMode::Motion;
    case Option::PointerCStick: return settings.pointer == PointerMode::CStick;
    case Option::PointerStylus: return settings.pointer == PointerMode::Stylus;
    case Option::InvertX: return settings.invertX;
    case Option::InvertY: return settings.invertY;
    default: return false;
    }
}

Input::Input(const Settings& settings) : settings_(settings) {}

Button Input::sample(TriggerChecker& checker, bool held) {
    checker.update(held);
    return {checker.getLevel(), checker.getOnTrigger(), checker.getOffTrigger()};
}

void Input::setSettings(const Settings& settings) {
    if (settings.pointer != settings_.pointer) {
        motion_.reset();
        recenter();
    }
    settings_ = settings;
}

bool Input::takeSettingsChanged() {
    const bool changed = settingsChanged_;
    settingsChanged_ = false;
    return changed;
}

void Input::recenter() {
    pointerX_ = (TopWidth - 1) / 2;
    pointerY_ = (TopHeight - 1) / 2;
}

void Input::setPointer(float x, float y) {
    pointerX_ = std::clamp(x, 0.0f, TopWidth - 1);
    pointerY_ = std::clamp(y, 0.0f, TopHeight - 1);
    frame_.pointer = {true, pointerX_, pointerY_,
                      2.0f * pointerX_ / (TopWidth - 1) - 1.0f, 1.0f - 2.0f * pointerY_ / (TopHeight - 1)};
}

void Input::beginGesture(const RawInput& raw) {
    const int x = raw.touchX, y = raw.touchY;
    const bool stylus = settings_.pointer == PointerMode::Stylus;
    gesture_ = Gesture::Ignored;
    gestureOption_ = -1;
    if (capturing_ >= 0) {
        // Any tap finishes choosing; buttons still held stay out of the game until released.
        capturing_ = -1;
        suppressed_ = previousHeld_;
        return;
    }
    if (remapOpen_) {
        if (OptionsButton.contains(x, y)) {
            remapOpen_ = false;
            gesture_ = Gesture::OptionsTab;
            return;
        }
        for (unsigned action = 0; action < ActionCount; ++action) {
            if (!remapRow(action).contains(x, y)) continue;
            gesture_ = Gesture::RemapRow;
            gestureOption_ = capturing_ = static_cast<int>(action);
            captureFresh_ = true;
            return;
        }
        return;
    }
    if (optionsOpen_) {
        for (const auto& button : OptionButtons) {
            if (!button.bounds.contains(x, y)) continue;
            gesture_ = Gesture::Option;
            gestureOption_ = static_cast<int>(button.option);
            if (button.option == Option::Done) {
                optionsOpen_ = false;
            } else if (button.option == Option::Remap) {
                remapOpen_ = true;
            } else if (const auto next = applyOption(button.option, settings_); next != settings_) {
                setSettings(next);
                settingsChanged_ = true;
            }
            break;
        }
        return;
    }
    if (OptionsButton.contains(x, y)) {
        gesture_ = Gesture::OptionsTab;
        optionsOpen_ = true;
    } else if (CameraButton.contains(x, y)) {
        gesture_ = Gesture::Camera;
    } else if (ShootButton.contains(x, y)) {
        gesture_ = stylus ? Gesture::Shoot : Gesture::Recenter;
        if (stylus) shootMode_ = !shootMode_;
    } else if (stylus && AimArea.contains(x, y)) {
        gesture_ = Gesture::Aim;
    }
}

const InputFrame& Input::update(const RawInput& raw, float seconds) {
    // A slow frame moves the pointer by at most a quarter second's worth: its rates are one moment's, not the frame's.
    const float dt = std::isfinite(seconds) && seconds > 0 ? std::min(seconds, 0.25f) : 0.0f;
    if (!raw.touching) {
        gesture_ = Gesture::None;
        gestureOption_ = -1;
        suppressUntilLift_ = false;
    } else if (!touching_ && !suppressUntilLift_) {
        beginGesture(raw);
    }
    touching_ = raw.touching;
    const bool touchActive = raw.touching && !suppressUntilLift_;
    const bool stylus = settings_.pointer == PointerMode::Stylus;

    std::uint32_t held = raw.held;
    const std::uint32_t pressed = held & ~previousHeld_;
    previousHeld_ = held;
    suppressed_ &= held;
    if (capturing_ >= 0) {
        if (pressed) {
            auto& binding = settings_.bindings[capturing_];
            binding = captureFresh_ ? pressed : binding | pressed;
            captureFresh_ = false;
            settingsChanged_ = true;
        }
        held = 0;
    }
    held &= ~suppressed_;
    Button actions[ActionCount];
    for (unsigned action = 0; action < ActionCount; ++action)
        actions[action] = sample(actions_[action], (held & settings_.bindings[action]) != 0);
    frame_ = {};
    frame_.pointerMode = settings_.pointer;
    frame_.jump = actions[Jump];
    frame_.spin = actions[Spin];
    frame_.crouch = actions[Crouch];
    frame_.pause = actions[Pause];
    frame_.minus = actions[Minus];
    frame_.dpadUp = sample(up_, (held & Up) != 0);
    frame_.dpadDown = sample(down_, (held & Down) != 0);
    frame_.dpadLeft = sample(left_, (held & Left) != 0);
    frame_.dpadRight = sample(right_, (held & Right) != 0);
    radial(raw.circleX, raw.circleY, 15.0f, 156.0f, frame_.moveX, frame_.moveY);
    float stickX, stickY;
    radial(raw.cstickX, raw.cstickY, 15.0f, 140.0f, stickX, stickY);

    frame_.cameraButtonHeld = touchActive && gesture_ == Gesture::Camera;
    frame_.shootButtonHeld = touchActive && gesture_ == Gesture::Shoot;
    frame_.recenterButtonHeld = touchActive && gesture_ == Gesture::Recenter;
    frame_.heldOption = touchActive && gesture_ == Gesture::Option ? gestureOption_ : -1;
    frame_.optionsButtonHeld = touchActive &&
        (gesture_ == Gesture::OptionsTab || frame_.heldOption == static_cast<int>(Option::Done));
    if (gesture_ == Gesture::RemapRow) frame_.heldOption = -1;
    frame_.camera = sample(camera_, actions[CameraReset].held || frame_.cameraButtonHeld);
    frame_.recenter = sample(recenter_, !stylus && (actions[PointerReset].held || frame_.recenterButtonHeld));
    if (frame_.recenter.pressed) recenter();

    if (stylus) {
        if (touchActive && gesture_ == Gesture::Aim) {
            // Clamp a captured aim drag to the field; never turn it into a UI press.
            const float u = std::clamp(raw.touchX, AimArea.x, AimArea.x + AimArea.width - 1) / 319.0f;
            const float v = std::clamp(raw.touchY, AimArea.y, AimArea.y + AimArea.height - 1) / 239.0f;
            setPointer(u * (TopWidth - 1), v * (TopHeight - 1));
        }
    } else if (settings_.pointer == PointerMode::Motion) {
        if (raw.motion) motion_.update(raw.gyro, raw.accel, dt);
        const float scale = PixelsPerDegree * settings_.motionSpeed / 100.0f * dt;
        const float yaw = raw.motion ? motion_.yawRate() : 0.0f, pitch = raw.motion ? motion_.pitchRate() : 0.0f;
        setPointer(pointerX_ + (settings_.invertX ? 1 : -1) * yaw * scale,
                   pointerY_ + (settings_.invertY ? 1 : -1) * pitch * scale);
    } else {
        // Squared response keeps small C-stick nudges precise.
        const float magnitude = std::sqrt(stickX * stickX + stickY * stickY);
        const float scale = StickPixelsPerSecond * settings_.stickSpeed / 100.0f * magnitude * dt;
        setPointer(pointerX_ + (settings_.invertX ? -1 : 1) * stickX * scale,
                   pointerY_ - (settings_.invertY ? -1 : 1) * stickY * scale);
    }
    if (settings_.pointer != PointerMode::CStick) {
        frame_.cameraX = stickX;
        frame_.cameraY = stickY;
    }
    frame_.shootMode = stylus && shootMode_;
    const bool touchShot = frame_.shootMode && touchActive && gesture_ == Gesture::Aim;
    // ZR is the Wii's B whether or not the pointer shows: the game asks for it at the title and in menus too.
    frame_.shoot = sample(shoot_, actions[Shoot].held || touchShot);
    frame_.optionsOpen = optionsOpen_;
    frame_.remapOpen = remapOpen_;
    frame_.capturing = capturing_;
    return frame_;
}

void Input::cancelGesture() {
    gesture_ = Gesture::None;
    gestureOption_ = -1;
    suppressUntilLift_ = true;
    if (frame_.pointerMode == PointerMode::Stylus) frame_.pointer.visible = false;
    frame_.shoot = {};
    frame_.camera = {};
    frame_.cameraButtonHeld = frame_.shootButtonHeld = frame_.recenterButtonHeld = false;
    frame_.optionsButtonHeld = false;
    frame_.heldOption = -1;
    shoot_.setInput(false);
    camera_.setInput(false);
}

WiiInput toWiiInput(const InputFrame& frame) {
    WiiInput result;
    const auto add = [&](const Button& button, std::uint32_t mask) {
        if (button.held) result.held |= mask;
        if (button.pressed) result.pressed |= mask;
        if (button.released) result.released |= mask;
    };
    add(frame.jump, WiiA);
    add(frame.shoot, WiiB);
    add(frame.crouch, WiiZ);
    add(frame.camera, WiiC);
    add(frame.pause, WiiPlus);
    add(frame.minus, WiiMinus);
    add(frame.dpadUp, WiiUp);
    add(frame.dpadDown, WiiDown);
    add(frame.dpadLeft, WiiLeft);
    add(frame.dpadRight, WiiRight);
    result.stickX = frame.moveX;
    result.stickY = frame.moveY;
    result.swing = frame.spin.held;
    result.swingTrigger = frame.spin.pressed;
    result.pointer = frame.pointer;
    return result;
}
} // namespace smg
