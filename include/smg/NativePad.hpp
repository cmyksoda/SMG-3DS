#pragma once
#include <smg/Input.hpp>
#include <smg/Kpad.hpp>
#include <Game/System/WPad.hpp>
#include <Game/System/WPadButton.hpp>
#include <Game/System/WPadHolder.hpp>
#include <Game/System/WPadStick.hpp>

namespace smg {
// Owns one local controller and supplies samples to Petari's original button/stick code.
class NativePad {
public:
    explicit NativePad(int channel = 0);
    ~NativePad();
    NativePad(const NativePad&) = delete;
    NativePad& operator=(const NativePad&) = delete;
    void update(const InputFrame&, double elapsedSeconds = 1.0/60.0);
    const WPadButton& buttons() const { return buttons_; }
    const WPadStick& stick() const { return stick_; }
    const WiiInput& sample() const { return sample_; }
    WPad& pad() { return pad_; }
private:
    WPad pad_;
    WPadButton buttons_;
    WPadStick stick_;
    WPadReadDataInfo readInfo_;
    KPADStatus status_{};
    WiiInput sample_;
    KpadRepeat repeat_;
};
}
