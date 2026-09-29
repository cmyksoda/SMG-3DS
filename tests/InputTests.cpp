#include <smg/Input.hpp>
#include <smg/Session.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {
unsigned checks = 0;
void check(bool passed, const char* description) {
    ++checks;
    if (!passed) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a-b) < 0.001f; }
smg::RawInput touch(int x, int y) {
    smg::RawInput r; r.touching = true; r.touchX = x; r.touchY = y; return r;
}
}
int main() {
    smg::Input input;
    smg::RawInput raw;
    check(!input.update(raw).pointer.visible, "no pointer without stylus");
    for (auto key : {smg::A, smg::B}) {
        raw.held = key;
        check(input.update(raw).jump.pressed, "both jump keys press Wii A");
        check(smg::toWiiInput(input.frame()).pressed == smg::WiiA, "3DS B does not fire star bits");
        check(!input.update(raw).jump.pressed, "held jump does not retrigger");
        raw.held = 0; check(input.update(raw).jump.released, "jump release");
    }
    raw.held = smg::A; input.update(raw);
    raw.held = smg::A | smg::B;
    check(!input.update(raw).jump.pressed, "alias press does not retrigger");
    raw.held = smg::B; check(!input.update(raw).jump.released, "alias handoff preserves held");
    raw = {}; input.update(raw);
    for (auto key : {smg::X, smg::Y}) {
        raw.held = key;
        check(input.update(raw).spin.pressed, "spin alias triggers");
        check(smg::toWiiInput(input.frame()).swingTrigger, "spin maps to shake trigger");
        raw = {}; input.update(raw);
    }
    for (auto key : {smg::L, smg::R}) {
        raw.held = key;
        check(input.update(raw).crouch.pressed, "crouch alias triggers");
        check(smg::toWiiInput(input.frame()).held == smg::WiiZ, "crouch maps to Z");
        raw = {}; input.update(raw);
    }
    raw.circleX = 10; check(near(input.update(raw).moveX, 0), "stick deadzone");
    raw.circleX = 156; check(near(input.update(raw).moveX, 1), "full horizontal travel");
    raw.circleY = 156;
    auto frame = input.update(raw);
    check(near(std::hypot(frame.moveX, frame.moveY), 1), "diagonal magnitude capped radially");
    raw = {}; input.update(raw);
    for (auto point : {touch(0, 0), touch(319, 0), touch(0, 239), touch(319, 239)}) {
        auto f = input.update(point);
        check(f.pointer.visible, "every top corner reachable");
        check(near(f.pointer.x, point.touchX == 0 ? 0 : 399), "horizontal screen mapping");
        check(near(f.pointer.y, static_cast<float>(point.touchY)), "vertical screen mapping");
        check(near(smg::touchButtonOpacity(f), 0.5f), "buttons half transparent while aiming");
        check(!input.update({}).pointer.visible, "pointer disappears immediately on lift");
    }
    input.update(touch(160, 120));
    frame = input.update(touch(280, 210));
    check(frame.pointer.visible && !frame.shootMode, "aiming over shoot button does not toggle");
    frame = input.update(touch(25, 210));
    check(frame.pointer.visible && !frame.camera.held, "aiming over camera button does not activate");
    input.update({});
    frame = input.update(touch(280, 210));
    check(frame.shootMode && !frame.pointer.visible && !frame.shoot.held, "toggle press consumes touch without firing");
    check(near(smg::touchButtonOpacity(frame), 1), "UI press stays opaque");
    check(input.update(touch(280, 210)).shootMode, "holding toggle toggles only once");
    check(!input.update(touch(160, 120)).pointer.visible, "UI gesture cannot leak into aim");
    input.update({});
    frame = input.update(touch(160, 120));
    check(frame.shoot.pressed && frame.pointer.visible, "shoot mode touch triggers fire");
    check(smg::toWiiInput(frame).pressed == smg::WiiB, "fire maps to Wii trigger B");
    check(!input.update(touch(160, 120)).shoot.pressed, "held firing preserves original B semantics");
    frame = input.update({}); check(frame.shoot.released && !frame.pointer.visible, "lift stops shooting");
    frame = input.update(touch(20, 205));
    check(frame.camera.pressed && !frame.shoot.held && frame.shootMode, "camera separate from shooting toggle");
    check(smg::toWiiInput(frame).pressed == smg::WiiC, "camera maps to C");
    check(!input.update(touch(20, 205)).camera.pressed, "camera is a momentary button");
    check(input.update({}).camera.released, "camera releases");
    input.update(touch(160, 120)); input.cancelGesture();
    check(!input.frame().pointer.visible, "suspend immediately hides pointer");
    check(!input.update(touch(280, 210)).pointer.visible, "suspend suppresses current touch");
    check(input.frame().shootMode, "suspend does not toggle shooting");
    input.update({}); check(input.update(touch(160, 120)).pointer.visible, "new touch works after resume and lift");
    input.update({}); input.update(touch(280, 210));
    check(!input.frame().shootMode, "second fresh toggle turns shooting off");
    input.update({}); raw.held = smg::Up | smg::Down | smg::Left | smg::Right;
    check(smg::toWiiInput(input.update(raw)).held == 15, "all D-pad directions passed to original camera logic");

    smg::Session session;
    check(!session.booted(), "original NerveExecutor starts with pending boot");
    session.updateNerve();
    check(session.booted() && session.getNerveStep() == 0, "original Spine switches Nerve and resets step");
    session.updateNerve();
    check(session.getNerveStep() == 1, "original Spine updates active Nerve");
    std::printf("%u input and Petari core checks passed\n", checks);
}
