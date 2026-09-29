#include <smg/Input.hpp>
#include <smg/NativePad.hpp>
#include <smg/Session.hpp>
#include <smg/SceneRenderer.hpp>
#include <smg/TouchUi.hpp>
#include <3ds.h>
#include <citro2d.h>

namespace {
bool suspendInput = false;
void aptCallback(APT_HookType type, void*) {
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONSLEEP || type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
        suspendInput = true;
}
smg::RawInput readInput() {
    hidScanInput();
    const u32 held = hidKeysHeld();
    smg::RawInput input;
    const struct { u32 native; smg::Key port; } bindings[] = {
        {KEY_A, smg::A}, {KEY_B, smg::B}, {KEY_X, smg::X}, {KEY_Y, smg::Y},
        {KEY_L, smg::L}, {KEY_R, smg::R}, {KEY_DUP, smg::Up}, {KEY_DDOWN, smg::Down},
        {KEY_DLEFT, smg::Left}, {KEY_DRIGHT, smg::Right}, {KEY_START, smg::Start}, {KEY_SELECT, smg::Select}
    };
    for (const auto& binding : bindings) if (held & binding.native) input.held |= binding.port;
    circlePosition circle;
    hidCircleRead(&circle);
    input.circleX = circle.dx; input.circleY = circle.dy;
    input.touching = (held & KEY_TOUCH) != 0;
    if (input.touching) {
        touchPosition touch;
        hidTouchRead(&touch);
        input.touchX = touch.px; input.touchY = touch.py;
    }
    return input;
}
}

int main() {
    gfxInitDefault();
    gfxSet3D(false);
    osSetSpeedupEnable(true);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) { gfxExit(); return 1; }
    if (!C2D_Init(C2D_DEFAULT_MAX_OBJECTS)) { C3D_Fini(); gfxExit(); return 1; }
    C2D_Prepare();
    smg::TouchUi ui;
    if (!ui.init()) { ui.shutdown(); C2D_Fini(); C3D_Fini(); gfxExit(); return 1; }
    smg::Input input;
    smg::NativePad pad;
    smg::Session session("sdmc:/3ds/SMG-3DS/game");
    smg::SceneRenderer scene;
    scene.load("sdmc:/3ds/SMG-3DS/game/ObjectData/BroadBeanPlanetB.arc","broadbeanplanetb.bdl");
    aptHookCookie hook;
    aptHook(&hook, aptCallback, nullptr);
    u64 previousTime = osGetTime();
    while (aptMainLoop()) {
        const auto raw = readInput();
        if ((raw.held & (smg::Start | smg::Select)) == (smg::Start | smg::Select)) break;
        const u64 now = osGetTime();
        double elapsed = now >= previousTime ? (now-previousTime)/1000.0 : 0.0;
        previousTime = now;
        if (suspendInput) { input.cancelGesture(); suspendInput = false; elapsed = 0; }
        const auto& frame = input.update(raw);
        pad.update(frame, elapsed);
        session.updateNerve();
        scene.update(frame,static_cast<float>(elapsed));
        ui.draw(frame, pad, scene, session.booted(), session.assetStatus().c_str());
    }
    aptUnhook(&hook);
    scene.shutdown();
    ui.shutdown();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
