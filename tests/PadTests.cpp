#include <smg/NativePad.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
unsigned checks = 0;
void check(bool value, const char* description) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
} // namespace

int runPadTests() {
    check(MR::getWPad(0) == nullptr && MR::getWPad(-1) == nullptr, "unbound and invalid channels");
    {
        smg::NativePad pad;
        smg::Settings stylus;
        stylus.pointer = smg::PointerMode::Stylus;
        smg::Input input(stylus);
        smg::RawInput raw;
        const auto sample = [&] { pad.update(input.update(raw)); };
        check(pad.pad().getKPadStatus(0) == nullptr, "no stale status before first sample");
        sample();
        check(MR::getWPad(0) == &pad.pad(), "original controller lookup");
        check(pad.pad().getValidStatusCount() == 1 && pad.pad().getKPadStatus(1) == nullptr, "bounded single sample");

        for (auto key : {smg::A, smg::B}) {
            raw.held = key;
            sample();
            check(pad.buttons().testButtonA() && pad.buttons().testTriggerA(), "jump aliases through original WPadButton");
            sample();
            check(!pad.buttons().testTriggerA(), "original held suppression");
            raw = {};
            sample();
            check(!pad.buttons().testButtonA(), "jump release");
        }
        raw.held = smg::A;
        sample();
        raw.held = smg::A | smg::B;
        sample();
        check(!pad.buttons().testTriggerA(), "alias addition has no second original trigger");
        raw.held = smg::B;
        sample();
        check(pad.buttons().testButtonA(), "alias handoff maintains original hold");
        raw = {};
        sample();

        raw.held = smg::ZL;
        sample();
        check(pad.buttons().testButtonZ() && pad.buttons().testTriggerZ(), "crouch uses original Z");
        raw.held = smg::ZL | smg::L;
        sample();
        check(!pad.buttons().testReleaseZ() && pad.buttons().testTriggerC(), "camera reset joins a held crouch");
        raw = {};
        sample();
        check(pad.buttons().testReleaseZ() && !pad.buttons().testButtonC(), "original Z and C release");
        raw.held = smg::ZR;
        sample();
        check(pad.buttons().testButtonB(), "ZR is the Wii's B with the stylus lifted, as the title's A + B needs");
        raw = {};
        sample();

        raw.held = smg::Up | smg::Left | smg::Start | smg::Select;
        sample();
        check(pad.buttons().testButtonUp() && pad.buttons().testTriggerLeft() &&
              pad.buttons().testTriggerPlus() && pad.buttons().testTriggerMinus(), "D-pad and menu bits");

        raw = {};
        sample();
        raw.circleX = 156;
        raw.circleY = 156;
        sample();
        check(std::fabs(pad.stick().mSpeed - 1) < 0.0001f, "original stick distance uses portable square root");
        check(pad.stick().mTrigger == 5 && pad.stick().mIsTriggerUp, "original stick directional triggers");
        sample();
        check(pad.stick().mTrigger == 0 && !pad.stick().isChanged(), "stable original stick");
        raw = {};
        sample();
        check(pad.stick().mRelease == 5, "original stick release");

        raw.touching = true;
        raw.touchX = 280;
        raw.touchY = 212;
        sample();
        check(!pad.buttons().testButtonB(), "toggle itself never fires");
        raw = {};
        sample();
        raw.touching = true;
        raw.touchX = 0;
        raw.touchY = 0;
        sample();
        check(pad.buttons().testTriggerB(), "shooting aim generates original B trigger");
        const auto* status = pad.pad().getKPadStatus(0);
        check(status->dpd_valid_fg == 2 && status->pos.x == -1 && status->pos.y == -1, "top left KPAD coordinates");
        raw.touchX = 319;
        raw.touchY = 239;
        sample();
        check(status->pos.x == 1 && status->pos.y == 1, "bottom right KPAD coordinates");
        raw.touchX = 280;
        raw.touchY = 212;
        sample();
        check(pad.buttons().testButtonB() && !pad.buttons().testTriggerB(), "pass over mode button keeps original firing hold");
        raw = {};
        sample();
        check(status->dpd_valid_fg == 0 && !pad.buttons().testButtonB(), "stylus lift immediately invalidates KPAD pointer");

        raw.touching = true;
        raw.touchX = 25;
        raw.touchY = 212;
        sample();
        check(pad.buttons().testTriggerC() && !pad.buttons().testButtonB(), "camera uses original C without shot");
        raw = {};
        sample();
        check(!pad.buttons().testButtonC(), "camera release");

        raw.held = smg::Y;
        sample();
        check(pad.sample().swingTrigger, "digital spin preserved at original input boundary");
        sample();
        check(pad.sample().swing && !pad.sample().swingTrigger, "digital spin hold");
        raw = {};
        sample();

        KPADSetBtnRepeat(0, 0.1f, 0.05f);
        raw.held = smg::Up;
        sample();
        pad.update(input.update(raw), 0.11);
        check((status->hold & KPAD_BUTTON_RPT) != 0 && !pad.buttons().testTriggerUp(), "repeat flag preserves physical edge");
        pad.update(input.update(raw), 0.01);
        check((status->hold & KPAD_BUTTON_RPT) == 0, "repeat interval gap");
        pad.update(input.update(raw), 0.05);
        check((status->hold & KPAD_BUTTON_RPT) != 0, "repeat pulse");
        raw = {};
        sample();
        check(status->hold == 0, "repeat release resets timer");
        KPADSetBtnRepeat(0, 0, 0);
        raw.held = smg::Up;
        sample();
        pad.update(input.update(raw), 10);
        check((status->hold & KPAD_BUTTON_RPT) == 0, "disabled repeat");
        KPADSetBtnRepeat(-1, 1, 1);
    }
    check(MR::getWPad(0) == nullptr, "controller lifetime clears lookup");
    std::printf("%u original WPad checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runPadTests(); }
#endif
