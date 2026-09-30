#include <smg/Settings.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace smg {
namespace {
const char* const keyNames[KeyCount]{
    "A", "B", "X", "Y", "L", "R", "Up", "Down", "Left", "Right", "Start", "Select", "ZL", "ZR"};
const char* const actionNames[ActionCount]{
    "jump", "spin", "crouch", "shoot", "camera", "recenter", "pause", "minus"};
const char* const pointerNames[]{"motion", "cstick", "stylus"};

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

template<std::size_t N>
int lookup(const char* const (&names)[N], const std::string& value) {
    for (std::size_t i = 0; i < N; ++i) if (value == names[i]) return static_cast<int>(i);
    return -1;
}

bool parseKeys(const std::string& value, std::uint32_t& keys) {
    keys = 0;
    if (value == "none") return true;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = std::min(value.find('+', start), value.size());
        const int bit = lookup(keyNames, trim(value.substr(start, end - start)));
        if (bit < 0) return false;
        keys |= 1u << bit;
        start = end + 1;
    }
    return true;
}

std::string formatKeys(std::uint32_t keys) {
    std::string text;
    for (unsigned bit = 0; bit < KeyCount; ++bit) {
        if (!(keys & (1u << bit))) continue;
        if (!text.empty()) text += '+';
        text += keyNames[bit];
    }
    return text.empty() ? "none" : text;
}

void parseSpeed(const std::string& value, int& speed) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (!value.empty() && !*end && parsed >= Settings::MinSpeed && parsed <= Settings::MaxSpeed)
        speed = static_cast<int>(parsed);
}
} // namespace

const char* keyName(unsigned bit) { return bit < KeyCount ? keyNames[bit] : "?"; }
const char* actionName(Action action) { return action < ActionCount ? actionNames[action] : "?"; }

Bindings defaultBindings() {
    Bindings bindings{};
    bindings[Jump] = A | B;
    bindings[Spin] = X | Y;
    bindings[Crouch] = ZL;
    bindings[Shoot] = ZR;
    bindings[CameraReset] = L;
    bindings[PointerReset] = R;
    bindings[Pause] = Start;
    bindings[Minus] = Select;
    return bindings;
}

bool operator==(const Settings& a, const Settings& b) {
    return a.pointer == b.pointer && a.bindings == b.bindings && a.motionSpeed == b.motionSpeed &&
           a.stickSpeed == b.stickSpeed && a.invertX == b.invertX && a.invertY == b.invertY;
}

Settings parseSettings(const std::string& text, Settings base) {
    Settings settings = base;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = std::min(text.find('\n', start), text.size());
        const auto line = trim(text.substr(start, end - start));
        start = end + 1;
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const auto key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
        if (key == "pointer") {
            if (const int mode = lookup(pointerNames, value); mode >= 0) settings.pointer = static_cast<PointerMode>(mode);
        } else if (key == "motion_speed") {
            parseSpeed(value, settings.motionSpeed);
        } else if (key == "stick_speed") {
            parseSpeed(value, settings.stickSpeed);
        } else if (key == "invert_x" || key == "invert_y") {
            if (value == "0" || value == "1") (key == "invert_x" ? settings.invertX : settings.invertY) = value == "1";
        } else if (key.rfind("bind.", 0) == 0) {
            const int action = lookup(actionNames, key.substr(5));
            std::uint32_t keys;
            if (action >= 0 && parseKeys(value, keys)) settings.bindings[action] = keys;
        }
    }
    return settings;
}

std::string formatSettings(const Settings& settings) {
    std::string text = "# SMG-3DS controls. Keys: A B X Y L R ZL ZR Up Down Left Right Start Select, joined with +.\n";
    text += "pointer=" + std::string(pointerNames[static_cast<int>(settings.pointer)]) + "\n";
    text += "motion_speed=" + std::to_string(settings.motionSpeed) + "\n";
    text += "stick_speed=" + std::to_string(settings.stickSpeed) + "\n";
    text += "invert_x=" + std::string(settings.invertX ? "1" : "0") + "\n";
    text += "invert_y=" + std::string(settings.invertY ? "1" : "0") + "\n";
    for (unsigned action = 0; action < ActionCount; ++action)
        text += "bind." + std::string(actionNames[action]) + "=" + formatKeys(settings.bindings[action]) + "\n";
    return text;
}

bool loadSettings(const char* path, Settings& settings) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::string text;
    char buffer[512];
    std::size_t read;
    while (text.size() < 16384 && (read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);
    settings = parseSettings(text, settings);
    return true;
}

bool saveSettings(const char* path, const Settings& settings) {
    // Finish the new file before replacing the old one, so a power cut leaves one of them whole.
    const std::string temporary = std::string(path) + ".new";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) return false;
    const auto text = formatSettings(settings);
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    if (std::fclose(file) != 0 || !written) { std::remove(temporary.c_str()); return false; }
    std::remove(path); // FAT rename does not replace an existing file.
    return std::rename(temporary.c_str(), path) == 0;
}
} // namespace smg
