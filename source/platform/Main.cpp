#include <smg/Input.hpp>
#include <smg/NativePad.hpp>
#include <smg/Session.hpp>
#include <smg/SceneRenderer.hpp>
#include <smg/TouchUi.hpp>
#include <3ds.h>
#include <citro2d.h>

namespace {
constexpr const char* SettingsPath = "sdmc:/3ds/SMG-3DS/settings.ini";
constexpr const char* DataRoot = "sdmc:/3ds/SMG-3DS/game";
// Hundreds of draws with their own combiner state overflow citro3d's default 256 KiB.
constexpr std::size_t CommandBufferSize = 0x100000;
bool suspendInput = false;

void aptCallback(APT_HookType type, void*) {
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
        suspendInput = true;
}

struct Motion {
    bool ready = false;
    float dpsPerRaw = 0;
    bool start() {
        float rawPerDps = 0;
        ready = R_SUCCEEDED(HIDUSER_EnableAccelerometer()) && R_SUCCEEDED(HIDUSER_EnableGyroscope()) &&
                R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&rawPerDps)) && rawPerDps > 0;
        dpsPerRaw = ready ? 1.0f / rawPerDps : 0;
        return ready;
    }
    void stop() {
        HIDUSER_DisableGyroscope();
        HIDUSER_DisableAccelerometer();
    }
};

smg::RawInput readInput(const Motion& motion) {
    hidScanInput();
    const u32 held = hidKeysHeld();
    smg::RawInput input;
    const struct { u32 native; smg::Key port; } bindings[] = {
        {KEY_A, smg::A}, {KEY_B, smg::B}, {KEY_X, smg::X}, {KEY_Y, smg::Y},
        {KEY_L, smg::L}, {KEY_R, smg::R}, {KEY_ZL, smg::ZL}, {KEY_ZR, smg::ZR},
        {KEY_DUP, smg::Up}, {KEY_DDOWN, smg::Down}, {KEY_DLEFT, smg::Left}, {KEY_DRIGHT, smg::Right},
        {KEY_START, smg::Start}, {KEY_SELECT, smg::Select}
    };
    for (const auto& binding : bindings) if (held & binding.native) input.held |= binding.port;
    circlePosition circle;
    hidCircleRead(&circle);
    input.circleX = circle.dx;
    input.circleY = circle.dy;
    hidCstickRead(&circle);
    input.cstickX = circle.dx;
    input.cstickY = circle.dy;
    input.touching = (held & KEY_TOUCH) != 0;
    if (input.touching) {
        touchPosition touch;
        hidTouchRead(&touch);
        input.touchX = touch.px;
        input.touchY = touch.py;
    }
    if (motion.ready) {
        angularRate rate;
        accelVector accel;
        hidGyroRead(&rate);
        hidAccelRead(&accel);
        // libctru declares the ring entry as {x, z, y}; keep the ring's own order.
        input.gyro[0] = rate.x * motion.dpsPerRaw;
        input.gyro[1] = rate.z * motion.dpsPerRaw;
        input.gyro[2] = rate.y * motion.dpsPerRaw;
        input.accel[0] = accel.x;
        input.accel[1] = accel.y;
        input.accel[2] = accel.z;
        input.motion = true;
    }
    return input;
}
} // namespace

int main() {
    // stderr goes to svcOutputDebugString, so a stop at a boundary is named in the emulator's log.
    consoleDebugInit(debugDevice_SVC);
    gfxInitDefault();
    gfxSet3D(false);
    osSetSpeedupEnable(true);
    if (!C3D_Init(CommandBufferSize)) { gfxExit(); return 1; }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { C3D_Fini(); gfxExit(); return 1; }
    C2D_Prepare();
    smg::TouchUi ui;
    if (!ui.init()) { ui.shutdown(); C2D_Fini(); C3D_Fini(); gfxExit(); return 1; }
    Motion motion;
    motion.start();
    smg::Settings settings;
    smg::loadSettings(SettingsPath, settings);
    smg::Input input(settings);
    smg::NativePad pad;
    smg::Session session(DataRoot);
    smg::SceneRenderer scene;
    if (scene.init()) scene.loadStage(DataRoot, settings.previewGalaxy, settings.previewScenario);
    aptHookCookie hook;
    aptHook(&hook, aptCallback, nullptr);
    u64 previousTime = osGetTime();
    while (aptMainLoop()) {
        const auto raw = readInput(motion);
        if ((raw.held & (smg::Start | smg::Select)) == (smg::Start | smg::Select)) break;
        const u64 now = osGetTime();
        double elapsed = now >= previousTime ? (now - previousTime) / 1000.0 : 0.0;
        previousTime = now;
        if (suspendInput) { input.cancelGesture(); suspendInput = false; elapsed = 0; }
        const auto& frame = input.update(raw, static_cast<float>(elapsed));
        if (input.takeSettingsChanged()) smg::saveSettings(SettingsPath, input.settings());
        pad.update(frame, elapsed);
        session.updateNerve();
        scene.update(frame, static_cast<float>(elapsed));
        ui.draw(frame, input.settings(), pad, scene, session.booted(), session.assetStatus().c_str(), motion.ready);
    }
    aptUnhook(&hook);
    motion.stop(); // also undoes a partial start
    scene.shutdown();
    ui.shutdown();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
