#include <smg/HidInput.hpp>
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
    smg::HidMotion motion;
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
        smg::RawInput raw = smg::readHidInput();
        motion.read(raw);
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
        ui.draw(frame, input.settings(), pad, scene, session.booted(), session.assetStatus().c_str(), motion.ready());
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
