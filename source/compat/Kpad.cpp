// The Wii Remote's processing library (KPAD) in the port. Channel 0 is the 3DS itself, read through the port's own
// mapping (smg::Input, smg::toWiiInput) as the native pad reads it; channels 1 to 3 have no controller. The 3DS's
// gyroscope moves the pointer in motion mode, as the Wii Remote's sensor bar and gyro do.
#include <smg/HidInput.hpp>
#include <smg/Kpad.hpp>
#include <algorithm>
#include <cstdio>

namespace {
struct Params {
    f32 playRadius = 0, sensitivity = 1;
};
// Kept as KPAD keeps them: they shape the Wii's own pointer and acceleration filtering, which the port's input does
// in its own way (smg::Input's settings).
Params sPos[WPAD_MAX_CONTROLLERS], sHori[WPAD_MAX_CONTROLLERS], sDist[WPAD_MAX_CONTROLLERS], sAcc[WPAD_MAX_CONTROLLERS];
f32 sSensorHeight[WPAD_MAX_CONTROLLERS];
smg::KpadRepeat sRepeat[WPAD_MAX_CONTROLLERS];
u64 sLastRead = 0;
smg::WiiInput sLatest;

// The player's pointer mode and button layout, as the native app saves them.
smg::Input& input() {
    static smg::Input port = [] {
        smg::Settings settings;
        smg::loadSettings(smg::SettingsPath, settings);
        // The game's screens don't draw the touch controls yet, so their corners would act unseen.
        return smg::Input(settings, false);
    }();
    return port;
}
smg::HidMotion sMotion;
bool validChannel(s32 channel) { return channel >= 0 && channel < WPAD_MAX_CONTROLLERS; }
} // namespace

void smgWpadReportConnections();  // source/compat/Wpad.cpp

extern "C" {
void KPADInit() {
    if (!sMotion.ready() && !sMotion.start())
        std::fprintf(stderr, "The 3DS's motion sensors didn't start, so the pointer moves by C-Stick or stylus only\n");
    KPADReset();
}

void KPADReset() {
    for (auto& repeat : sRepeat) repeat.reset();
    input().cancelGesture();
    sLastRead = 0;
}

void KPADSetBtnRepeat(s32 channel, f32 delay, f32 pulse) {
    if (validChannel(channel)) sRepeat[channel].set(delay, pulse);
}
void KPADSetSensorHeight(s32 channel, f32 level) {
    if (validChannel(channel)) sSensorHeight[channel] = level;
}
void KPADSetAccParam(s32 channel, f32 playRadius, f32 sensitivity) {
    if (validChannel(channel)) sAcc[channel] = {playRadius, sensitivity};
}
void KPADSetPosParam(s32 channel, f32 playRadius, f32 sensitivity) {
    if (validChannel(channel)) sPos[channel] = {playRadius, sensitivity};
}
void KPADSetHoriParam(s32 channel, f32 playRadius, f32 sensitivity) {
    if (validChannel(channel)) sHori[channel] = {playRadius, sensitivity};
}
void KPADSetDistParam(s32 channel, f32 playRadius, f32 sensitivity) {
    if (validChannel(channel)) sDist[channel] = {playRadius, sensitivity};
}

// Development only (settings.ini dev.skip_story=1): adapted ProloguePictureBook and MoviePlayingSequence end at once.
BOOL smgDevSkipStory() { return input().settings().devSkipStory; }
// Development only (dev.gx_dump=1): the renderer lists a frame's draws now and then (source/compat/GxRender.cpp).
BOOL smgDevGxDump() { return input().settings().devGxDump; }
BOOL smgDevGxNull() { return input().settings().devGxNull; }
BOOL smgDevGxTiming() { return input().settings().devGxTiming; }
BOOL smgDevGxCheck() { return input().settings().devGxCheck; }
BOOL smgDevGxInline() { return input().settings().devGxInline; }
BOOL smgDevGxNoRaw() { return input().settings().devGxNoRaw; }
BOOL smgDevGxMips() { return input().settings().devGxMips; }
BOOL smgDevGxZones() { return input().settings().devGxZones; }
BOOL smgDevAvSync() { return input().settings().devAvSync; }
// dev.start: the start point Mario takes in `galaxy`, or -1 for the game's own.
int smgDevMarioStart(const char* galaxy) {
    const smg::Settings& s = input().settings();
    return s.devStartMario >= 0 && galaxy && s.devStartGalaxy == galaxy ? s.devStartMario : -1;
}
// dev.start's zone, whose StartInfo holds that start point: null for the galaxy's own.
const char* smgDevMarioZone(const char* galaxy) {
    const smg::Settings& s = input().settings();
    return smgDevMarioStart(galaxy) >= 0 && !s.devStartZone.empty() ? s.devStartZone.c_str() : nullptr;
}
// dev.galaxy: the stage and scenario the game's first stage gives way to, or null for the game's own.
const char* smgDevGalaxy(int* scenario) {
    const smg::Settings& s = input().settings();
    if (s.devGalaxyScenario <= 0) return nullptr;
    *scenario = s.devGalaxyScenario;
    return s.devGalaxy.c_str();
}
// dev.warp: where Mario is moved and how many seconds after he first appears, false when unset.
BOOL smgDevWarp(float* to, int* seconds) {
    const smg::Settings& s = input().settings();
    if (s.devWarp) std::copy(s.devWarpTo, s.devWarpTo + 3, to), *seconds = s.devWarpSeconds;
    return s.devWarp;
}
// GameSystem::frameLoop (adapted): logic steps per drawn frame. The Wii drew each one; the port draws every second one,
// 30 frames a second with the game at full speed, unless dev.draw_every=1.
int smgRenderInterval() { return input().settings().devDrawEvery ? 1 : 2; }

// In stylus mode the pointer is on the screen only while the stylus touches it (adapted StarPointerGuidance asks).
BOOL smgPointerIsStylus() { return input().settings().pointer == smg::PointerMode::Stylus; }

// The 3DS has no remote to shake: the spin buttons are its swing (adapted WPadHVSwing::update asks).
BOOL smgSpinHeld() { return sLatest.swing; }
BOOL smgSpinPressed() { return sLatest.swingTrigger; }

// One sample per read, newest first as KPAD returns them; the count read, 0 for a channel with no controller.
s32 KPADRead(s32 channel, KPADStatus* statuses, u32 count) {
    if (channel != 0 || count == 0) return 0;
    smgWpadReportConnections();
    const u64 now = osGetTime();
    const double seconds = sLastRead ? (now - sLastRead) / 1000.0 : 1.0 / 60.0;
    sLastRead = now;
    smg::RawInput raw = smg::readHidInput();
    sMotion.read(raw);
    const smg::WiiInput sample = smg::toWiiInput(input().update(raw, static_cast<float>(seconds)));
    sLatest = sample;
    smg::writeKpadStatus(sample, statuses[0]);
    if (sRepeat[channel].update(sample.held, seconds)) statuses[0].hold |= KPAD_BUTTON_RPT;
    return 1;
}
}
