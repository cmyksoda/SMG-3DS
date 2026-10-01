#include <smg/AudioData.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::printf("FAIL: %s\n", message); std::exit(1); }
}
using Bytes = std::vector<std::uint8_t>;
void put32(Bytes& b, std::uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s)); }
std::uint32_t n32(const std::uint8_t* at) { std::uint32_t v; std::memcpy(&v, at, 4); return v; }
std::uint16_t n16(const std::uint8_t* at) { std::uint16_t v; std::memcpy(&v, at, 2); return v; }

void meTable() {
    // Two entries, then their names: the disc's metable.bmt has this shape (40 of them).
    Bytes b;
    put32(b, 2), put32(b, 16), put32(b, 32), put32(b, 0);
    b.insert(b.end(), {100, 101, 64, 5, 0x01, 0x2C, 7, 8, 1, 2, 3, 4, 0x00, 0x3C, 5, 6});
    put32(b, 40), put32(b, 43);
    b.insert(b.end(), {'M', 'A', 0, 'M', 'B', 0});
    check(!smg::nativeMeTable(b.data(), b.size()), "a music-event table turns native");
    check(n32(b.data()) == 2 && n32(b.data() + 4) == 16 && n32(b.data() + 8) == 32, "its header reads as the holder's s32s");
    check(b[16] == 100 && b[19] == 5 && n16(b.data() + 16 + 4) == 300 && b[22] == 7 && n16(b.data() + 24 + 4) == 60,
          "each entry's bytes stay, its 16-bit field reads natively");
    check(n32(b.data() + 32) == 40 && n32(b.data() + 36) == 43 && !std::strcmp(reinterpret_cast<char*>(b.data() + 43), "MB"),
          "the name offsets read natively, for the game to relocate");
    Bytes bad = {0, 0, 0, 9, 0, 0, 0, 16, 0, 0, 0, 32};
    check(smg::nativeMeTable(bad.data(), bad.size()) != nullptr, "a table larger than its file is refused");
}

void meSequences() {
    Bytes b;
    put32(b, 2), put32(b, 12), put32(b, 14);
    b.insert(b.end(), {0xC1, 0x00, 0xC2, 0x00});
    check(!smg::nativeMeSequences(b.data(), b.size()), "a music-event sequence file turns native");
    check(n32(b.data()) == 2 && n32(b.data() + 4) == 12 && n32(b.data() + 8) == 14 && b[12] == 0xC1, "its starts read natively, its bytecode stays");
}
// The disc's smallest chord table, smg_astrodome02_midi.cit: one chord, one scale.
void chordTable() {
    Bytes b;
    put32(b, 0), b.insert(b.end(), {'C', 'I', 'T', 'S'}), put32(b, 0x40);
    b.insert(b.end(), {0, 1, 0, 1});
    put32(b, 0x18), put32(b, 0x20);
    b.insert(b.end(), {8, 3, 8, 10, 0x7F, 0x7F, 0x7F, 0x7F});
    put32(b, 0x28), put32(b, 0x34);
    b.resize(0x40, 0x7F);
    check(!smg::nativeChordTable(b.data(), b.size()), "a chord table turns native");
    check(n32(b.data()) == 0 && n32(b.data() + 8) == 0x40 && n16(b.data() + 12) == 1 && n16(b.data() + 14) == 1,
          "its flag stays unrelocated, its size and counts read natively");
    check(n32(b.data() + 16) == 0x18 && n32(b.data() + 20) == 0x20 && n32(b.data() + 0x20) == 0x28 && n32(b.data() + 0x24) == 0x34,
          "its chord and scale offsets and each scale's two note offsets read natively");
    check(b[0x18] == 8 && b[0x1B] == 10, "a chord's notes stay bytes");
    check(smg::nativeChordTable(b.data(), b.size()) != nullptr, "one the game relocated is not turned twice");
}

void remixSequence() {
    // One group of two tracks with one note each.
    Bytes b;
    put32(b, 1), put32(b, 12), put32(b, 2), put32(b, 1), put32(b, 0x30);
    for (unsigned t = 0; t < 2; ++t) {
        put32(b, t);
        for (unsigned f = 0; f < 4; ++f) put32(b, 0x100 * t + f);
    }
    check(!smg::nativeRemixSequence(b.data(), b.size()), "a remix sequence turns native");
    check(n32(b.data()) == 1 && n32(b.data() + 8) == 2 && n32(b.data() + 16) == 0x30 && n32(b.data() + 4 * 11) == 0x100 && n32(b.data() + 4 * 14) == 0x103,
          "every word of it reads natively");
    Bytes bad = {0, 0, 0, 1, 0, 0, 0, 4, 0, 0, 0, 9, 0, 0, 0, 1};
    check(smg::nativeRemixSequence(bad.data(), bad.size()) != nullptr, "one whose tracks run past its end is refused");
}
// The disc's spktable.bct and spkwave.csw have these shapes (34 sounds).
void speaker() {
    Bytes t;
    put32(t, 1), put32(t, 16), put32(t, 24), put32(t, 0);
    t.insert(t.end(), {0x00, 0x05, 0x78, 0x78, 0x01, 0x2C, 0, 0});
    put32(t, 28), t.insert(t.end(), {'S', 0});
    check(!smg::nativeSpeakerTable(t.data(), t.size()), "a speaker table turns native");
    check(n32(t.data()) == 1 && n32(t.data() + 12) == 0 && n16(t.data() + 16) == 5 && t[18] == 0x78 && n16(t.data() + 20) == 300 &&
              n32(t.data() + 24) == 28,
          "its header, 16-bit fields and name offset read natively, its bytes stay");
    Bytes w;
    put32(w, 0), put32(w, 1), put32(w, 12);
    put32(w, 4), put32(w, 0xFFFFFFFF), put32(w, 0xFFFFFFFF);
    w.insert(w.end(), {0x12, 0x34, 0xFF, 0xFE});
    check(!smg::nativeSpeakerWaves(w.data(), w.size()), "speaker waves turn native");
    check(n32(w.data() + 4) == 1 && n32(w.data() + 8) == 12 && n32(w.data() + 12) == 4 && n16(w.data() + 24) == 0x1234 &&
              n16(w.data() + 26) == 0xFFFE,
          "their offsets, sizes and PCM16 samples read natively");
}
float f32(const std::uint8_t* at) { float v; std::memcpy(&v, at, 4); return v; }
void putTag(Bytes& b, const char* text) { b.insert(b.end(), text, text + 4); }
void putFloat(Bytes& b, float value) { std::uint32_t v; std::memcpy(&v, &value, 4); put32(b, v); }
void align(Bytes& b, std::size_t to) { b.resize((b.size() + to - 1) / to * to, 0); }

// An archive with one of each part SMG's has, in its shapes: a sound table, its names, a sequence collection whose
// group table sits at an odd offset, a wave system and a version 1 bank.
void audioArchive() {
    Bytes b;
    putTag(b, "AA_<");
    const std::size_t commands = b.size();
    b.resize(commands + 4 * (3 + 3 + 3 + 4 + 3) + 4, 0);  // bst, bstn, bsc, ws, bnk, >_AA
    auto command = [&](std::size_t at, const char* name, std::initializer_list<std::uint32_t> args) {
        std::memcpy(&b[at], name, 4);
        std::size_t i = at + 4;
        for (std::uint32_t arg : args) b[i] = arg >> 24, b[i + 1] = arg >> 16 & 0xFF, b[i + 2] = arg >> 8 & 0xFF, b[i + 3] = arg & 0xFF, i += 4;
        return i;
    };

    align(b, 32);
    const std::size_t bst = b.size();
    putTag(b, "BST "), put32(b, 0), put32(b, 0x01000000), put32(b, 0x10);
    put32(b, 1), put32(b, 0x18);                       // root: one section
    put32(b, 1), put32(b, 0x20);                       // section: one group
    put32(b, 2), put32(b, 0), put32(b, 0x50000030), put32(b, 0x70000038);  // group: an SE, a stream
    b.insert(b.end(), {0x80, 0x7F, 0x00, 0x05, 0x02, 0x00, 0, 0});           // SE: priority, volume, 5, 0x200
    b.insert(b.end(), {0x40, 0x60, 0x00, 0x03}), put32(b, 0x40);            // stream: channels 3, its name
    b.insert(b.end(), {'a', '.', 'a', 's', 't', 0});
    const std::size_t bstEnd = b.size();

    align(b, 32);
    const std::size_t bstn = b.size();
    putTag(b, "BSTN"), put32(b, 0), put32(b, 0x01000000), put32(b, 0x10);
    put32(b, 1), put32(b, 0x18);
    put32(b, 1), put32(b, 0), put32(b, 0x24);
    put32(b, 1), put32(b, 0x30), put32(b, 0x33);
    b.insert(b.end(), {'S', 'E', 0, 'S', 'E', '_', 'A', 0});
    const std::size_t bstnEnd = b.size();

    align(b, 32);
    const std::size_t bsc = b.size();
    b.insert(b.end(), {'S', 'C', 0x00, 0x01}), put32(b, 0x20), put32(b, 0x0D);
    b.push_back(0xC1);                                 // a sequence byte, then the group table at +0x0D
    put32(b, 1), put32(b, 0x0C);
    const std::size_t bscEnd = b.size();

    align(b, 32);
    const std::size_t ws = b.size();
    auto to = [&](std::size_t offset) { b.resize(ws + offset, 0); };
    putTag(b, "WSYS"), put32(b, 0x108), put32(b, 7), put32(b, 1), put32(b, 0x20), put32(b, 0x30);
    to(0x20), putTag(b, "WINF"), put32(b, 1), put32(b, 0x40);                // archive bank
    to(0x30), putTag(b, "WBCT"), put32(b, 0), put32(b, 1), put32(b, 0xC0);   // control group
    to(0x40), b.insert(b.end(), {'w', '.', 'a', 'w', 0});                     // archive: a name, a count, one wave
    to(0xB0), put32(b, 1), put32(b, 0xD0);
    to(0xC0), putTag(b, "SCNE"), put32(b, 0), put32(b, 0), put32(b, 0xF8);   // scene
    to(0xD0), b.insert(b.end(), {0, 3, 60, 0}), putFloat(b, 32000.0f), put32(b, 0x100), put32(b, 0x200), put32(b, 0xFFFFFFFF);
    put32(b, 0), put32(b, 0x3FF), put32(b, 0x400), b.insert(b.end(), {0x12, 0x34, 0xFF, 0xFE});
    to(0xF8), putTag(b, "C-DF"), put32(b, 1), put32(b, 0x104);               // control
    put32(b, 0x00020009);                                                     // its wave: group 2, wave 9
    const std::size_t wsEnd = b.size();

    align(b, 32);
    const std::size_t bnk = b.size();
    putTag(b, "IBNK"), put32(b, 0), put32(b, 42), put32(b, 1);
    b.resize(bnk + 0x20, 0);
    putTag(b, "ENVT"), put32(b, 6), b.insert(b.end(), {0, 1, 0, 2, 0x7F, 0xFF}), align(b, 4);
    putTag(b, "OSCT"), put32(b, 4 + 0x1C), put32(b, 1);
    putTag(b, "Osci"), b.insert(b.end(), {1, 0, 0, 0}), putFloat(b, 1.0f), put32(b, 0), put32(b, 0), putFloat(b, 1.0f), putFloat(b, 0.0f);
    const std::size_t list = b.size() - bnk;
    putTag(b, "LIST"), put32(b, 8), put32(b, 1), put32(b, static_cast<std::uint32_t>(list + 0x10));
    b.resize(bnk + list + 0x10, 0);
    putTag(b, "Inst"), put32(b, 1), put32(b, 0), put32(b, 0), put32(b, 1);  // one oscillator (index 0), no effects, one key
    put32(b, 0x7F000000), put32(b, 1);                 // high key 0x7F, one velocity region
    b.insert(b.end(), {0x7F, 0, 0, 0}), put32(b, 0x00010009), putFloat(b, 1.0f), putFloat(b, 2.0f);
    putFloat(b, 0.5f), putFloat(b, 1.5f);
    const std::size_t bnkEnd = b.size();
    b[bnk + 4] = 0, b[bnk + 5] = 0, b[bnk + 6] = static_cast<std::uint8_t>((bnkEnd - bnk) >> 8), b[bnk + 7] = static_cast<std::uint8_t>(bnkEnd - bnk);

    std::size_t at = commands;
    at = command(at, "bst ", {std::uint32_t(bst), std::uint32_t(bstEnd)});
    at = command(at, "bstn", {std::uint32_t(bstn), std::uint32_t(bstnEnd)});
    at = command(at, "bsc ", {std::uint32_t(bsc), std::uint32_t(bscEnd)});
    at = command(at, "ws  ", {0, std::uint32_t(ws), 0});
    at = command(at, "bnk ", {0, std::uint32_t(bnk)});
    command(at, ">_AA", {});
    (void)wsEnd;

    const Bytes original = b;
    const char* why = smg::nativeAudioArchive(b.data(), b.size());
    if (why) std::printf("  (%s)\n", why);
    check(!why, "an audio archive turns native");
    check(std::memcmp(b.data(), original.data(), bst) == 0, "its command stream stays big-endian for the interpreter");
    check(n32(&b[bst]) == 0x42535420 && n32(&b[bst + 0x28]) == 0x50000030 && n16(&b[bst + 0x32]) == 5 && n16(&b[bst + 0x34]) == 0x200 &&
              b[bst + 0x30] == 0x80 && n16(&b[bst + 0x3A]) == 3 && n32(&b[bst + 0x3C]) == 0x40,
          "the sound table's magic, item words and item fields read natively");
    check(n32(&b[bstn]) == 0x4253544E && n32(&b[bstn + 0x2C]) == 0x33 && b[bstn + 0x33] == 'S', "the name table's offsets read natively");
    check(n16(&b[bsc + 2]) == 1 && n32(&b[bsc + 8]) == 0x0D && b[bsc + 0x0D] == 0 && b[bsc + 0x10] == 1,
          "the collection's header reads natively, its odd group table stays big-endian");
    check(f32(&b[ws + 0xD4]) == 32000.0f && n32(&b[ws + 0xD8]) == 0x100 && n32(&b[ws + 0xE8]) == 0x3FF && n16(&b[ws + 0xF0]) == 0x1234 &&
              (n32(&b[ws + 0x104]) & 0xFFFF) == 9 && b[ws + 0xD1] == 3,
          "a wave's rate, offsets, loop, samples and id read natively, its bytes stay");
    check(n32(&b[bnk + 8]) == 42 && n16(&b[bnk + 0x2C]) == 0x7FFF && f32(&b[bnk + 0x44]) == 1.0f,
          "the bank's number, envelope points and oscillator floats read natively");
    const std::size_t inst = bnk + list + 0x10;
    check(n32(&b[inst]) == 0x496E7374 && n32(&b[inst + 0x14]) >> 24 == 0x7F && b[inst + 0x1C] == 0x7F && (n32(&b[inst + 0x20]) & 0xFFFF) == 9 &&
              f32(&b[inst + 0x28]) == 2.0f && f32(&b[inst + 0x2C]) == 0.5f,
          "an instrument's kind, key, velocity region and volume read natively");

    Bytes twice = b;
    check(smg::nativeAudioArchive(twice.data(), twice.size()) != nullptr, "an archive already turned is refused, not turned back");
}
} // namespace

// A sound animation as Balloonfish.arc's appearance.bas has it: one sound, ID 0x0009020B at frame 80, pitch 1.
void soundAnimation() {
    Bytes b;
    b.push_back(0), b.push_back(1), b.push_back(0), b.push_back(0), put32(b, 0);
    float values[3] = {80.0f, 80.0f, 1.0f};
    put32(b, 0x0009020B);
    for (float v : values) { std::uint32_t w; std::memcpy(&w, &v, 4); put32(b, w); }
    put32(b, 0x80);  // plays at intervals
    for (unsigned byte : {0x7Fu, 0u, 0u, 0x40u, 0u, 0u, 0u, 0u}) b.push_back(static_cast<std::uint8_t>(byte));
    put32(b, 0);
    check(!smg::nativeSoundAnimation(b.data(), b.size()), "a sound animation turns native");
    float on, pitch;
    std::memcpy(&on, b.data() + 8 + 4, 4), std::memcpy(&pitch, b.data() + 8 + 0xC, 4);
    check(n16(b.data()) == 1 && n32(b.data() + 8) == 0x0009020B && on == 80.0f && pitch == 1.0f && n32(b.data() + 8 + 0x10) == 0x80 &&
              b[8 + 0x14] == 0x7F && b[8 + 0x17] == 0x40, "its count, sound ID, frames, pitch and flags read natively; its bytes stay");
    Bytes cut(b.begin(), b.begin() + 20);
    check(smg::nativeSoundAnimation(cut.data(), cut.size()) != nullptr, "a cut-off one is refused");
}

int runAudioDataTests() {
    soundAnimation();
    audioArchive();
    speaker();
    meTable();
    meSequences();
    chordTable();
    remixSequence();
    std::printf("%u audio data checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runAudioDataTests(); }
#endif
