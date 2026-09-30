#include <smg/NativePad.hpp>
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
    smg::writeKpadStatus(sample_, status_);
}
NativePad::~NativePad() { pads[pad_.mChannel] = nullptr; }

void NativePad::update(const InputFrame& frame, double elapsedSeconds) {
    sample_ = toWiiInput(frame);
    writeKpadStatus(sample_, status_);
    repeat_.set(repeat[pad_.mChannel].delay, repeat[pad_.mChannel].pulse);
    if (repeat_.update(sample_.held, elapsedSeconds)) status_.hold |= KPAD_BUTTON_RPT;
    readInfo_.mValidStatusCount = 1;
    buttons_.update();
    stick_.update();
}
}
