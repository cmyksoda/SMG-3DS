#include <smg/NativePad.hpp>
#include <cmath>
#include <cstdlib>

namespace {
struct Repeat { double delay = 0, pulse = 0; } repeat[WPAD_MAX_CONTROLLERS];
smg::NativePad* pads[WPAD_MAX_CONTROLLERS]{};
bool validChannel(int channel) { return channel >= 0 && channel < WPAD_MAX_CONTROLLERS; }
}

extern "C" void KPADSetBtnRepeat(s32 channel, f32 delay, f32 pulse) {
    if (!validChannel(channel)) return;
    repeat[channel] = {delay, pulse};
}

WPadReadDataInfo::WPadReadDataInfo() : mStatusArray(nullptr), mValidStatusCount(0) {}
KPADStatus* WPadReadDataInfo::getKPadStatus(u32 index) const {
    return index < mValidStatusCount ? &mStatusArray[index] : nullptr;
}
u32 WPadReadDataInfo::getValidStatusCount() const { return mValidStatusCount; }

WPad::WPad(s32 channel)
    : mChannel(channel), mReadInfo(nullptr), mButton(nullptr), mPointer(nullptr),
      mCorePadAccel(nullptr), mCorePadSwing(nullptr), _18(nullptr), _1C(nullptr),
      mStick(nullptr), mSubPadAccel(nullptr), mSubPadSwing(nullptr), mLeaveWatcher(nullptr),
      mInfoChecker(nullptr), _34(false), mIsConnected(false), mIsSubPadConnected(false) {}
void WPad::setReadInfo(WPadReadDataInfo* info) { mReadInfo = info; }
KPADStatus* WPad::getKPadStatus(u32 index) const { return mReadInfo ? mReadInfo->getKPadStatus(index) : nullptr; }
s32 WPad::getValidStatusCount() const { return mReadInfo ? mReadInfo->getValidStatusCount() : 0; }

namespace MR {
bool isDeviceFreeStyle(const KPADStatus* status) {
    return status && status->wpad_err == WPAD_ERR_NONE && status->dev_type == WPAD_DEV_FREESTYLE;
}
WPad* getWPad(s32 channel) {
    return validChannel(channel) && pads[channel] ? &pads[channel]->pad() : nullptr;
}
}

namespace smg {
NativePad::NativePad(int channel) : pad_(channel), buttons_(&pad_), stick_(&pad_) {
    // Duplicate ownership is a programming error: it must not silently redirect game input.
    if (!validChannel(channel) || pads[channel]) std::abort();
    pads[channel] = this;
    pad_.mButton = &buttons_;
    pad_.mStick = &stick_;
    readInfo_.mStatusArray = &status_;
    pad_.setReadInfo(&readInfo_);
    pad_.mIsConnected = pad_.mIsSubPadConnected = true;
    status_.dev_type = WPAD_DEV_FREESTYLE;
    status_.data_format = WPAD_FMT_FREESTYLE_ACC_DPD;
    status_.wpad_err = WPAD_ERR_NONE;
}
NativePad::~NativePad() { pads[pad_.mChannel] = nullptr; }

void NativePad::update(const InputFrame& frame, double elapsedSeconds) {
    const auto previous = sample_.held;
    sample_ = toWiiInput(frame);
    status_.hold = sample_.held;
    status_.trig = sample_.pressed;
    status_.release = sample_.released;
    status_.ex_status.fs.stick = {sample_.stickX, sample_.stickY};
    // KPAD uses screen coordinates (+Y downward); our rendering NDC uses +Y upward.
    status_.pos = {sample_.pointer.ndcX, -sample_.pointer.ndcY};
    status_.dpd_valid_fg = sample_.pointer.visible ? 2 : 0;
    status_.horizon = {1, 0};
    const auto settings = repeat[pad_.mChannel];
    if (previous != sample_.held || sample_.held == 0) {
        repeatElapsed_ = 0;
        nextRepeat_ = settings.delay;
    } else if (std::isfinite(elapsedSeconds) && elapsedSeconds > 0 &&
               std::isfinite(settings.delay) && settings.delay >= 0 &&
               std::isfinite(settings.pulse) && settings.pulse > 0) {
        repeatElapsed_ += elapsedSeconds;
        if (repeatElapsed_ >= nextRepeat_) {
            status_.hold |= KPAD_BUTTON_RPT;
            nextRepeat_ += (std::floor((repeatElapsed_-nextRepeat_)/settings.pulse)+1)*settings.pulse;
        }
    }
    readInfo_.mValidStatusCount = 1;
    buttons_.update();
    stick_.update();
}
}
