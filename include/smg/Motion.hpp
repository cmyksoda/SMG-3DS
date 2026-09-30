#pragma once

namespace smg {
// Gyro aiming in "player space": sideways motion follows turning about the real vertical,
// whatever angle the console is held at. Axes are the HID ring's order: pitch, yaw, roll.
class MotionTracker {
public:
    // gyro in degrees per second; accel only needs the right direction, any scale.
    void update(const float gyro[3], const float accel[3], float seconds);
    void reset();
    float yawRate() const { return yaw_; }     // positive turns left
    float pitchRate() const { return pitch_; } // positive tips the top screen toward the player
    const float* up() const { return up_; }
private:
    float up_[3]{0, 1, 0};
    float bias_[3]{};
    float stillSeconds_ = 0;
    float yaw_ = 0, pitch_ = 0;
};
} // namespace smg
