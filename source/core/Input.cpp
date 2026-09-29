#include <smg/Input.hpp>
#include <algorithm>
#include <cmath>

namespace smg {
Button Input::sample(TriggerChecker& checker, bool held) {
    checker.update(held);
    return {checker.getLevel(), checker.getOnTrigger(), checker.getOffTrigger()};
}

const InputFrame& Input::update(const RawInput& raw) {
    const auto isHeld = [&](std::uint32_t keys) { return (raw.held & keys) != 0; };
    frame_ = {};
    frame_.jump = sample(jump_, isHeld(A | B));
    frame_.spin = sample(spin_, isHeld(X | Y));
    frame_.crouch = sample(crouch_, isHeld(L | R));
    frame_.pause = sample(pause_, isHeld(Start));
    frame_.select = sample(select_, isHeld(Select));
    frame_.dpadUp = sample(up_, isHeld(Up));
    frame_.dpadDown = sample(down_, isHeld(Down));
    frame_.dpadLeft = sample(left_, isHeld(Left));
    frame_.dpadRight = sample(right_, isHeld(Right));

    const float x = static_cast<float>(raw.circleX);
    const float y = static_cast<float>(raw.circleY);
    const float radius = std::sqrt(x*x + y*y);
    constexpr float deadzone = 15.0f;
    constexpr float fullTravel = 156.0f;
    if (radius > deadzone) {
        const float magnitude = std::min(1.0f, (radius - deadzone) / (fullTravel - deadzone));
        frame_.moveX = x / radius * magnitude;
        frame_.moveY = y / radius * magnitude;
    }

    if (!raw.touching) {
        gesture_ = Gesture::None;
        suppressUntilLift_ = false;
    } else if (!touching_ && !suppressUntilLift_) {
        if (CameraButton.contains(raw.touchX, raw.touchY)) gesture_ = Gesture::Camera;
        else if (ShootButton.contains(raw.touchX, raw.touchY)) {
            gesture_ = Gesture::Shoot;
            shootMode_ = !shootMode_;
        } else if (AimArea.contains(raw.touchX, raw.touchY)) gesture_ = Gesture::Aim;
        else gesture_ = Gesture::Ignored;
    }
    touching_ = raw.touching;
    frame_.shootMode = shootMode_;
    frame_.cameraButtonHeld = raw.touching && gesture_ == Gesture::Camera && !suppressUntilLift_;
    frame_.shootButtonHeld = raw.touching && gesture_ == Gesture::Shoot && !suppressUntilLift_;
    if (raw.touching && gesture_ == Gesture::Aim && !suppressUntilLift_) {
        // Clamp a captured aim drag to the field; never turn it into a UI press.
        const float u = std::clamp(raw.touchX, AimArea.x, AimArea.x + AimArea.width - 1) / 319.0f;
        const float v = std::clamp(raw.touchY, AimArea.y, AimArea.y + AimArea.height - 1) / 239.0f;
        frame_.pointer = {true, u * 399.0f, v * 239.0f, 2.0f*u - 1.0f, 1.0f - 2.0f*v};
    }
    frame_.camera = sample(camera_, frame_.cameraButtonHeld);
    frame_.shoot = sample(shoot_, shootMode_ && frame_.pointer.visible);
    return frame_;
}

void Input::cancelGesture() {
    gesture_ = Gesture::None;
    suppressUntilLift_ = true;
    frame_.pointer.visible = false;
    frame_.shoot = {};
    frame_.camera = {};
    frame_.cameraButtonHeld = frame_.shootButtonHeld = false;
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
    add(frame.select, WiiMinus);
    add(frame.dpadUp, WiiUp); add(frame.dpadDown, WiiDown);
    add(frame.dpadLeft, WiiLeft); add(frame.dpadRight, WiiRight);
    result.stickX = frame.moveX; result.stickY = frame.moveY;
    result.swing = frame.spin.held; result.swingTrigger = frame.spin.pressed;
    result.pointer = frame.pointer;
    return result;
}
} // namespace smg
