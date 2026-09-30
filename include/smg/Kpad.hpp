#pragma once
#include <smg/Input.hpp>
#include <revolution/kpad.h>

namespace smg {
// The port's controller as KPAD reports a Remote with a Nunchuk: buttons, stick and pointer from one input sample.
void writeKpadStatus(const WiiInput& input, KPADStatus& status);

// KPADSetBtnRepeat's repeat: after `delay` seconds of an unchanged hold, one repeat every `pulse` seconds.
class KpadRepeat {
public:
    void set(double delay, double pulse) { delay_ = delay; pulse_ = pulse; }
    // Forgets the hold in progress; the delay and pulse stay.
    void reset() { elapsed_ = next_ = 0; previous_ = 0; }
    // Whether this sample repeats, `elapsedSeconds` after the last.
    bool update(std::uint32_t held, double elapsedSeconds);
private:
    double delay_ = 0, pulse_ = 0, elapsed_ = 0, next_ = 0;
    std::uint32_t previous_ = 0;
};
} // namespace smg
