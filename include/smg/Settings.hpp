#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace smg {
enum Key : std::uint32_t {
    A = 1u << 0, B = 1u << 1, X = 1u << 2, Y = 1u << 3,
    L = 1u << 4, R = 1u << 5, Up = 1u << 6, Down = 1u << 7,
    Left = 1u << 8, Right = 1u << 9, Start = 1u << 10, Select = 1u << 11,
    ZL = 1u << 12, ZR = 1u << 13
};
inline constexpr unsigned KeyCount = 14;
const char* keyName(unsigned bit);

// Remappable game actions; the D-pad and both sticks keep fixed roles.
enum Action : unsigned { Jump, Spin, Crouch, Shoot, CameraReset, PointerReset, Pause, Minus, ActionCount };
const char* actionName(Action action);
using Bindings = std::array<std::uint32_t, ActionCount>;
// The Switch release's layout, so its players can pick this up without relearning.
Bindings defaultBindings();

enum class PointerMode : std::uint8_t { Motion, CStick, Stylus };

struct Settings {
    PointerMode pointer = PointerMode::Motion;
    Bindings bindings = defaultBindings();
    int motionSpeed = 100, stickSpeed = 100; // percent
    bool invertX = false, invertY = false;
    // The galaxy and scenario the stage preview opens; a galaxy is a StageData folder name.
    std::string previewGalaxy = "EggStarGalaxy";
    int previewScenario = 1;
    // For development only (dev.skip_story=1): the prologue's picture book and every movie end as soon as they start.
    bool devSkipStory = false;
    // For development only (dev.gx_dump=1): the renderer lists one frame's draws in the log every minute.
    bool devGxDump = false;
    // For development only (dev.gx_null=1): the renderer draws nothing, so a frame's time is the game's own (the command
    // processor still runs, raising draw-done and tokens).
    bool devGxNull = false;
    // For development only (dev.gx_timing=1): the perf line times the renderer's parts (each reading of the clock
    // costs Azahar 150 cycles, so this slows what it measures a little).
    bool devGxTiming = false;
    // For development only (dev.gx_check=1): each new mesh is checked against the CPU path, vertex by vertex.
    bool devGxCheck = false;
    // For development only (dev.gx_inline=1): the GX stream is drawn on the game's thread, not the render thread.
    bool devGxInline = false;
    // For development only (dev.draw_every=1): every logic step is drawn, as on the Wii, instead of every second one
    // (30 frames a second with the game at full speed).
    bool devDrawEvery = false;
    // For development only (dev.gx_noraw=1): every draw goes through citro3d, none written into the command list directly.
    bool devGxNoRaw = false;
    // For development only (dev.av_sync=1): every 90th drawn frame gets a white square and the audio a click at the
    // same moment, to measure the port's sound against its picture in a recording.
    bool devAvSync = false;
    // For development only (dev.start=GALAXY N): Mario starts that galaxy at its start point N (StartInfo's MarioNo),
    // to reach a stage's later parts without walking there.
    std::string devStartGalaxy;
    int devStartMario = -1;
    // For development only (dev.galaxy=GALAXY N): the game's first stage is that galaxy's scenario N instead, to reach
    // later scenes without playing up to them.
    std::string devGalaxy;
    int devGalaxyScenario = 0;
    // For development only (dev.warp=X Y Z): two seconds after Mario first appears, he's moved there (once a run).
    bool devWarp = false;
    float devWarpTo[3] = {};
    static constexpr int MinSpeed = 25, MaxSpeed = 300, SpeedStep = 25;
};
bool operator==(const Settings& a, const Settings& b);
inline bool operator!=(const Settings& a, const Settings& b) { return !(a == b); }

// Plain key=value text; unknown keys and bad values are skipped so old files keep loading.
Settings parseSettings(const std::string& text, Settings base = {});
std::string formatSettings(const Settings& settings);
// Where the 3DS keeps the player's settings, for the native app and the game alike.
inline constexpr const char* SettingsPath = "sdmc:/3ds/SMG-3DS/settings.ini";
bool loadSettings(const char* path, Settings& settings);
bool saveSettings(const char* path, const Settings& settings);
} // namespace smg
