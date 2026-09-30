// The Wii's system settings (SC), answered from the 3DS's own. The console language and sound output come
// from its settings; the TV settings describe the 3DS's screens.
#include <3ds.h>
#include <revolution/sc.h>

namespace {
bool configReady() {
    static const bool ready = R_SUCCEEDED(cfguInit());
    return ready;
}
} // namespace

extern "C" {
// The 3DS's languages in the Wii's numbering; Portuguese and Russian, which the Wii lacks, read as English.
u8 SCGetLanguage() {
    u8 language = CFG_LANGUAGE_EN;
    if (configReady()) CFGU_GetSystemLanguage(&language);
    switch (language) {
    case CFG_LANGUAGE_JP: return SC_LANG_JAPANESE;
    case CFG_LANGUAGE_FR: return SC_LANG_FRENCH;
    case CFG_LANGUAGE_DE: return SC_LANG_GERMAN;
    case CFG_LANGUAGE_IT: return SC_LANG_ITALIAN;
    case CFG_LANGUAGE_ES: return SC_LANG_SPANISH;
    case CFG_LANGUAGE_ZH: return SC_LANG_SIMP_CHINESE;
    case CFG_LANGUAGE_KO: return SC_LANG_KOREAN;
    case CFG_LANGUAGE_NL: return SC_LANG_DUTCH;
    case CFG_LANGUAGE_TW: return SC_LANG_TRAD_CHINESE;
    default: return SC_LANG_ENGLISH;
    }
}

// The top screen is 5:3, far nearer the Wii's widescreen than its 4:3.
u8 SCGetAspectRatio() { return SC_ASPECT_RATIO_16x9; }

// The 3DS's sound setting (config block 0x70001) has the Wii's three modes in the same order.
u8 SCGetSoundMode() {
    u8 mode = SC_SOUND_MODE_STEREO;
    if (configReady()) CFGU_GetConfigInfoBlk2(sizeof mode, 0x70001, &mode);
    return mode <= SC_SOUND_MODE_SURROUND ? mode : SC_SOUND_MODE_STEREO;
}

// TV output modes: the 3DS's screens are neither progressive component video nor PAL.
u8 SCGetProgressiveMode() { return 0; }
u8 SCGetEuRgb60Mode() { return 0; }
}
