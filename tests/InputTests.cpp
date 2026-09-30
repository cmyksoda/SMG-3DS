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
bool near(float a, float b, float tolerance = 0.001f) { return std::fabs(a - b) < tolerance; }
smg::RawInput touch(int x, int y) {
    smg::RawInput r;
    r.touching = true;
    r.touchX = x;
    r.touchY = y;
    return r;
}
smg::Settings withPointer(smg::PointerMode mode) {
    smg::Settings settings;
    settings.pointer = mode;
    return settings;
}
// Flat on a table in the HID frame: gravity along -Y, the bottom screen's normal.
smg::RawInput motion(float pitch, float yaw, float roll, float ax = 0, float ay = -512, float az = 0) {
    smg::RawInput r;
    r.motion = true;
    r.gyro[0] = pitch; r.gyro[1] = yaw; r.gyro[2] = roll;
    r.accel[0] = ax; r.accel[1] = ay; r.accel[2] = az;
    return r;
}
const smg::InputFrame& run(smg::Input& input, const smg::RawInput& raw, unsigned frames) {
    for (unsigned i = 1; i < frames; ++i) input.update(raw);
    return input.update(raw);
}

void defaultLayout() {
    smg::Input input;
    smg::RawInput raw;
    check(input.update(raw).pointer.visible, "motion pointer is always shown");
    check(near(input.frame().pointer.x, 199.5f) && near(input.frame().pointer.y, 119.5f), "pointer starts centered");
    for (auto key : {smg::A, smg::B}) {
        raw.held = key;
        check(input.update(raw).jump.pressed, "both jump keys press Wii A");
        check(smg::toWiiInput(input.frame()).pressed == smg::WiiA, "jump alone is only Wii A");
        check(!input.update(raw).jump.pressed, "held jump does not retrigger");
        raw.held = 0;
        check(input.update(raw).jump.released, "jump release");
    }
    raw.held = smg::A;
    input.update(raw);
    raw.held = smg::A | smg::B;
    check(!input.update(raw).jump.pressed, "alias press does not retrigger");
    raw.held = smg::B;
    check(!input.update(raw).jump.released, "alias handoff preserves held");
    raw = {};
    input.update(raw);
    for (auto key : {smg::X, smg::Y}) {
        raw.held = key;
        check(input.update(raw).spin.pressed, "spin alias triggers");
        check(smg::toWiiInput(input.frame()).swingTrigger, "spin maps to shake trigger");
        raw = {};
        input.update(raw);
    }
    const struct { std::uint32_t key, wii; const char* description; } buttons[]{
        {smg::ZL, smg::WiiZ, "ZL crouches with Nunchuk Z"},
        {smg::ZR, smg::WiiB, "ZR shoots star bits with Wii B"},
        {smg::L, smg::WiiC, "L resets the camera with Nunchuk C"},
        {smg::Start, smg::WiiPlus, "Start pauses with Wii Plus"},
        {smg::Select, smg::WiiMinus, "Select is Wii Minus"},
    };
    for (const auto& button : buttons) {
        raw.held = button.key;
        check(smg::toWiiInput(input.update(raw)).pressed == button.wii, button.description);
        raw = {};
        input.update(raw);
    }
    raw.held = smg::R;
    check(smg::toWiiInput(input.update(raw)).held == 0 && input.frame().recenter.pressed, "R recenters without a Wii button");
    raw = {};
    input.update(raw);

    raw.circleX = 10;
    check(near(input.update(raw).moveX, 0), "stick deadzone");
    raw.circleX = 156;
    check(near(input.update(raw).moveX, 1), "full horizontal travel");
    raw.circleY = 156;
    const auto& frame = input.update(raw);
    check(near(std::hypot(frame.moveX, frame.moveY), 1), "diagonal magnitude capped radially");
    raw = {};
    raw.cstickX = 140;
    check(near(input.update(raw).cameraX, 1) && near(input.frame().pointer.x, 199.5f), "C-stick is the camera stick in motion mode");
    raw = {};
    raw.held = smg::Up | smg::Down | smg::Left | smg::Right;
    check(smg::toWiiInput(input.update(raw)).held == 15, "all D-pad directions passed to original camera logic");

    smg::Settings custom;
    custom.bindings[smg::Shoot] = smg::ZR | smg::R;
    custom.bindings[smg::PointerReset] = smg::Select;
    smg::Input remapped(custom);
    raw = {};
    raw.held = smg::R;
    check(remapped.update(raw).shoot.pressed && !remapped.frame().recenter.held, "remapped R shoots instead of recentering");
    raw.held = smg::Select;
    check(remapped.update(raw).recenter.pressed, "remapped recenter");
}

void motionPointer() {
    smg::Input input;
    const auto& turned = run(input, motion(0, 10, 0), 60);
    check(near(turned.pointer.x, 199.5f - 10 * 400.0f / 36, 2) && near(turned.pointer.y, 119.5f), "turning left moves the pointer left");
    smg::RawInput recenter = motion(0, 0, 0);
    recenter.held = smg::R;
    check(near(input.update(recenter).pointer.x, 199.5f), "R recenters the motion pointer");
    input.update(motion(0, 0, 0));
    check(run(input, motion(10, 0, 0), 30).pointer.y < 119.5f - 40, "tipping the top screen back aims up");
    check(run(input, motion(-400, 0, 0), 60).pointer.y == 239, "pointer is held at the screen edge");
    input.update(recenter);

    // Held at 45 degrees, turning about the real vertical mixes the yaw and roll gyro axes.
    const float s = std::sqrt(0.5f);
    smg::Input tilted;
    run(tilted, motion(0, 0, 0, 0, -512 * s, 512 * s), 60);
    const auto& player = run(tilted, motion(0, 10 * s, -10 * s, 0, -512 * s, 512 * s), 60);
    check(near(player.pointer.x, 199.5f - 10 * 400.0f / 36, 3), "turning is measured about gravity when tilted");
    smg::Input flipped;
    run(flipped, motion(0, 0, 0, 0, 512, 0), 30);
    check(run(flipped, motion(0, 10, 0, 0, 512, 0), 60).pointer.x < 100, "gravity sign conventions give the same turn");

    smg::Settings inverted;
    inverted.invertX = inverted.invertY = true;
    inverted.motionSpeed = 200;
    smg::Input fast(inverted);
    const auto& invertedFrame = run(fast, motion(4, 4, 0), 30);
    check(invertedFrame.pointer.x > 199.5f + 40 && invertedFrame.pointer.y > 119.5f + 40, "invert and speed settings");

    smg::Input drifting;
    run(drifting, motion(0, 1.2f, 0), 600);
    const float before = drifting.frame().pointer.x;
    check(near(run(drifting, motion(0, 1.2f, 0), 60).pointer.x, before, 0.5f), "resting gyro offset is learned");
    check(run(drifting, motion(0, 20, 0), 30).pointer.x < before - 60, "real turns still move after learning");
    check(smg::toWiiInput(drifting.frame()).pointer.visible, "motion pointer reaches the original KPAD boundary");
}

void stickPointer() {
    smg::Input input(withPointer(smg::PointerMode::CStick));
    smg::RawInput raw;
    raw.cstickX = 140;
    const auto& frame = run(input, raw, 15);
    check(near(frame.pointer.x, 199.5f + 105, 1) && frame.cameraX == 0, "C-stick drives the pointer, not the camera");
    raw.cstickX = 0;
    raw.cstickY = 50;
    const float y = input.update(raw).pointer.y;
    check(y < 119.5f && y > 118.5f, "small C-stick nudges move slowly and up is up");
    raw = {};
    raw.held = smg::R;
    check(near(input.update(raw).pointer.x, 199.5f), "R recenters the C-stick pointer");
}

void stylusPointer() {
    smg::Input input(withPointer(smg::PointerMode::Stylus));
    smg::RawInput raw;
    check(!input.update(raw).pointer.visible, "no stylus pointer without a touch");
    for (auto point : {touch(0, 0), touch(319, 0), touch(0, 239), touch(319, 239)}) {
        const auto& f = input.update(point);
        check(f.pointer.visible, "every top corner reachable");
        check(near(f.pointer.x, point.touchX == 0 ? 0 : 399), "horizontal screen mapping");
        check(near(f.pointer.y, static_cast<float>(point.touchY)), "vertical screen mapping");
        check(near(smg::touchButtonOpacity(f), 0.5f), "buttons half transparent while aiming");
        check(!input.update({}).pointer.visible, "pointer disappears immediately on lift");
    }
    input.update(touch(160, 120));
    auto frame = input.update(touch(280, 210));
    check(frame.pointer.visible && !frame.shootMode, "aiming over shoot button does not toggle");
    frame = input.update(touch(25, 210));
    check(frame.pointer.visible && !frame.camera.held, "aiming over camera button does not activate");
    frame = input.update(touch(280, 10));
    check(frame.pointer.visible && !frame.optionsOpen, "aiming over the options tab does not open it");
    raw.held = smg::ZR;
    raw.touching = true;
    raw.touchX = 160;
    raw.touchY = 120;
    check(input.update(raw).shoot.pressed, "ZR shoots at the stylus pointer");
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
    frame = input.update({});
    check(frame.shoot.released && !frame.pointer.visible, "lift stops shooting");
    frame = input.update(touch(20, 205));
    check(frame.camera.pressed && !frame.shoot.held && frame.shootMode, "camera separate from shooting toggle");
    check(smg::toWiiInput(frame).pressed == smg::WiiC, "camera maps to C");
    check(!input.update(touch(20, 205)).camera.pressed, "camera is a momentary button");
    check(input.update({}).camera.released, "camera releases");
    input.update(touch(160, 120));
    input.cancelGesture();
    check(!input.frame().pointer.visible, "suspend immediately hides pointer");
    check(!input.update(touch(280, 210)).pointer.visible, "suspend suppresses current touch");
    check(input.frame().shootMode, "suspend does not toggle shooting");
    input.update({});
    check(input.update(touch(160, 120)).pointer.visible, "new touch works after resume and lift");
    input.update({});
    input.update(touch(280, 210));
    check(!input.frame().shootMode, "second fresh toggle turns shooting off");
}

void touchPages() {
    smg::Input input;
    auto frame = input.update(touch(284, 212));
    check(frame.recenterButtonHeld && frame.recenter.pressed && !frame.shootMode, "corner button recenters outside stylus mode");
    input.update({});
    check(near(input.update(touch(10, 100)).pointer.x, 199.5f), "touching the field does not aim in motion mode");
    input.update({});
    frame = input.update(touch(280, 12));
    check(frame.optionsOpen && frame.optionsButtonHeld, "options tab opens the options page");
    check(input.update(touch(280, 12)).optionsOpen, "holding the tab does not close it again");
    input.update({});
    frame = input.update(touch(270, 50));
    check(frame.pointerMode == smg::PointerMode::Stylus && frame.heldOption == static_cast<int>(smg::Option::PointerStylus),
          "pointer chip switches mode");
    check(input.takeSettingsChanged() && !input.takeSettingsChanged(), "a change is reported once");
    input.update({});
    check(!input.update(touch(160, 120)).pointer.visible, "stylus cannot aim through the options page");
    input.update({});
    input.update(touch(290, 85));
    input.update({});
    check(input.settings().motionSpeed == 125, "speed step");
    for (int i = 0; i < 20; ++i) { input.update(touch(100, 85)); input.update({}); }
    check(input.settings().motionSpeed == smg::Settings::MinSpeed, "speed floor");
    input.update(touch(120, 160));
    input.update({});
    check(input.settings().invertX && !input.settings().invertY, "horizontal invert toggle");
    input.update(touch(280, 12));
    check(!input.frame().optionsOpen, "done closes the options page");
    input.update({});
    check(input.update(touch(160, 120)).pointer.visible, "stylus aims again once closed");
}

void remapPage() {
    smg::Input input;
    input.update(touch(280, 12));
    input.update({});
    input.update(touch(250, 205));
    input.update({});
    check(input.frame().remapOpen && input.frame().optionsOpen, "remap chip opens the remap page");
    auto frame = input.update(touch(100, 34 + 3 * 24 + 5));
    check(frame.capturing == smg::Shoot, "a row starts choosing buttons for its action");
    input.update({});
    smg::RawInput raw;
    raw.held = smg::R;
    frame = input.update(raw);
    check(!frame.recenter.pressed && !frame.shoot.pressed, "buttons pressed while choosing never reach the game");
    check(input.settings().bindings[smg::Shoot] == smg::R, "the first press replaces the old binding");
    raw.held = smg::R | smg::ZR;
    input.update(raw);
    check(input.settings().bindings[smg::Shoot] == (smg::R | smg::ZR), "later presses add aliases");
    raw.touching = true;
    raw.touchX = raw.touchY = 100;
    frame = input.update(raw);
    check(frame.capturing == -1 && input.takeSettingsChanged(), "a tap finishes and reports the change");
    raw.touching = false;
    check(!input.update(raw).shoot.pressed, "buttons held through the finishing tap stay suppressed");
    input.update({});
    raw = {};
    raw.held = smg::R;
    check(input.update(raw).shoot.pressed, "the new binding works once pressed again");
    input.update({});
    input.update(touch(280, 12));
    input.update({});
    check(!input.frame().remapOpen && input.frame().optionsOpen, "back returns to the options page");
}

void settingsFiles() {
    smg::Settings settings;
    settings.pointer = smg::PointerMode::CStick;
    settings.motionSpeed = 150;
    settings.invertY = true;
    settings.bindings[smg::Shoot] = smg::ZR | smg::R;
    settings.bindings[smg::Minus] = 0;
    check(smg::parseSettings(smg::formatSettings(settings)) == settings, "settings round trip");
    const auto parsed = smg::parseSettings(
        "pointer=teleport\nmotion_speed=9000\nstick_speed=50\ninvert_x=yes\nbind.jump=A+Q\nbind.spin = Y \nnonsense\n");
    check(parsed.pointer == smg::PointerMode::Motion && parsed.motionSpeed == 100 && parsed.stickSpeed == 50,
          "bad values keep their defaults");
    check(!parsed.invertX && parsed.bindings[smg::Jump] == (smg::A | smg::B) && parsed.bindings[smg::Spin] == smg::Y,
          "bad bindings skipped, good ones trimmed");
}
} // namespace

int main() {
    defaultLayout();
    motionPointer();
    stickPointer();
    stylusPointer();
    touchPages();
    remapPage();
    settingsFiles();

    smg::Session session;
    check(!session.booted(), "original NerveExecutor starts with pending boot");
    session.updateNerve();
    check(session.booted() && session.getNerveStep() == 0, "original Spine switches Nerve and resets step");
    session.updateNerve();
    check(session.getNerveStep() == 1, "original Spine updates active Nerve");
    std::printf("%u input and Petari core checks passed\n", checks);
}
