#include <smg/Kpad.hpp>
#include <cmath>

namespace smg {
void writeKpadStatus(const WiiInput& input, KPADStatus& status) {
    status.hold = input.held;
    status.trig = input.pressed;
    status.release = input.released;
    status.ex_status.fs.stick = {input.stickX, input.stickY};
    // KPAD uses screen coordinates (+Y downward); our rendering NDC uses +Y upward.
    status.pos = {input.pointer.ndcX, -input.pointer.ndcY};
    status.dpd_valid_fg = input.pointer.visible ? 2 : 0;
    status.horizon = {1, 0};
    status.dev_type = WPAD_DEV_FREESTYLE;
    status.data_format = WPAD_FMT_FREESTYLE_ACC_DPD;
    status.wpad_err = WPAD_ERR_NONE;
}

bool KpadRepeat::update(std::uint32_t held, double elapsedSeconds) {
    const bool changed = held != previous_;
    previous_ = held;
    if (changed || held == 0) {
        elapsed_ = 0;
        next_ = delay_;
        return false;
    }
    if (!(std::isfinite(elapsedSeconds) && elapsedSeconds > 0 && std::isfinite(delay_) && delay_ >= 0 &&
          std::isfinite(pulse_) && pulse_ > 0))
        return false;
    elapsed_ += elapsedSeconds;
    if (elapsed_ < next_) return false;
    next_ += (std::floor((elapsed_ - next_) / pulse_) + 1) * pulse_;
    return true;
}
} // namespace smg
