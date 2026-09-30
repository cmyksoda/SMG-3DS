#include <smg/Motion.hpp>
#include <algorithm>
#include <cmath>

namespace smg {
namespace {
constexpr float StillRate = 3.0f;      // deg/s; slower than any deliberate aim
constexpr float SettleSeconds = 0.5f;
constexpr float TightenRate = 1.5f;    // deg/s; sensor noise below this is scaled toward zero
constexpr float YawRelax = 1.41f;
constexpr float GravitySeconds = 0.15f;

float tighten(float rate) {
    const float magnitude = std::fabs(rate);
    return magnitude < TightenRate ? rate * magnitude / TightenRate : rate;
}
} // namespace

void MotionTracker::reset() {
    *this = MotionTracker{};
}

void MotionTracker::update(const float gyro[3], const float accel[3], float seconds) {
    yaw_ = pitch_ = 0;
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 0.25f) return;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(gyro[i]) || !std::isfinite(accel[i])) return;

    // Gravity's sign convention differs between sources; the bottom screen faces up while playing.
    const float length = std::sqrt(accel[0] * accel[0] + accel[1] * accel[1] + accel[2] * accel[2]);
    if (length > 1e-6f) {
        const float sign = accel[1] > 0 ? 1.0f : -1.0f;
        const float blend = 1.0f - std::exp(-seconds / GravitySeconds);
        float next[3], nextLength = 0;
        for (int i = 0; i < 3; ++i) {
            next[i] = up_[i] + (sign * accel[i] / length - up_[i]) * blend;
            nextLength += next[i] * next[i];
        }
        nextLength = std::sqrt(nextLength);
        if (nextLength > 1e-6f) for (int i = 0; i < 3; ++i) up_[i] = next[i] / nextLength;
    }

    float rate[3];
    bool still = true;
    for (int i = 0; i < 3; ++i) {
        rate[i] = gyro[i] - bias_[i];
        still &= std::fabs(rate[i]) < StillRate;
    }
    // Learn the zero-rate offset only while the console has been held still for a moment.
    stillSeconds_ = still ? stillSeconds_ + seconds : 0;
    if (stillSeconds_ > SettleSeconds) {
        const float blend = std::min(1.0f, seconds / 2.0f);
        for (int i = 0; i < 3; ++i) bias_[i] += rate[i] * blend;
    }

    const float worldYaw = rate[1] * up_[1] + rate[2] * up_[2];
    const float localYaw = std::sqrt(rate[1] * rate[1] + rate[2] * rate[2]);
    yaw_ = tighten(std::copysign(std::min(std::fabs(worldYaw) * YawRelax, localYaw), worldYaw));
    pitch_ = tighten(rate[0]);
}
} // namespace smg
