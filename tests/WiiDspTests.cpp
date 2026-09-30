#include <smg/WiiDsp.hpp>
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

std::vector<std::uint8_t> memory(0x10000);
unsigned interrupts = 0;
std::uint8_t* at(std::uint32_t address, std::uint32_t size) { return address + size <= memory.size() ? memory.data() + address : nullptr; }
void put16(std::uint32_t address, std::uint16_t value) { std::memcpy(at(address, 2), &value, 2); }
void put32(std::uint32_t address, std::uint32_t value) { std::memcpy(at(address, 4), &value, 4); }
std::int16_t get16(std::uint32_t address) { std::int16_t v; std::memcpy(&v, at(address, 2), 2); return v; }

// Where this test keeps things, as JAudio2 would: voice blocks, its coefficient tables, "ARAM", outputs.
constexpr std::uint32_t Voices = 0x0000, Tables = 0x6000, Afc = 0x6600, Reverb = 0x6700, AramBase = 0x8000, Left = 0xF000, Right = 0xF400;

void expect(smg::WiiDsp& dsp, std::initializer_list<std::uint32_t> mails, const char* message) {
    bool same = true;
    for (std::uint32_t mail : mails) same &= dsp.mailWaiting() && dsp.readMail() == mail;
    check(same && !dsp.mailWaiting(), message);
}

// DSPSendCommands2: the count, then the words.
void command(smg::WiiDsp& dsp, std::initializer_list<std::uint32_t> words) {
    dsp.send(static_cast<std::uint32_t>(words.size()));
    for (std::uint32_t word : words) dsp.send(word);
}

void protocol() {
    smg::WiiDsp dsp({at, [] { ++interrupts; }});
    dsp.reset();
    expect(dsp, {0x8071FEED}, "the ROM says it is ready");
    for (std::uint32_t mail : {0x80F3A001u, 0x1234u, 0x80F3C002u, 0u, 0x80F3A002u, 0x1C00u, 0x80F3B002u, 0u, 0x80F3D001u, 0x10u}) dsp.send(mail);
    check(interrupts == 1, "booting the task interrupts the CPU once");
    expect(dsp, {0xDCD10000, 0xF3551111}, "the microcode's init mail, then its handshake");

    command(dsp, {0x8E000000, AramBase});
    expect(dsp, {0xDCD10004, 0xF3558E00}, "setting the ARAM base is acknowledged by its sync value");
    check(interrupts == 2, "each acknowledgement interrupts");

    // DSPRES_FILTER as 32-bit words: frac 0's four taps are {0, 0x7FFF, 0, 0}, so a 1:1 voice passes nearly unchanged.
    put32(Tables, 0x00007FFF), put32(Tables + 4, 0x00000000);
    command(dsp, {0x81000040, Voices, Tables, Afc, Reverb});
    expect(dsp, {0xDCD10004, 0xF3558100}, "setup is acknowledged");

    // Voice 0: 0x30 big-endian PCM16 samples of 4096 in "ARAM", not looping, sent at full volume to the front left.
    const std::uint32_t v = Voices;
    put16(v + 0x000, 1), put16(v + 0x004, 0x1000), put16(v + 0x008, 1);
    put16(v + 0x010, 0x0D00), put16(v + 0x012, 0x7FFF), put16(v + 0x014, 0x7FFF);
    put16(v + 0x100, 16), put32(v + 0x114, 0x30), put32(v + 0x118, 0x1000), put32(v + 0x068, 0);
    for (unsigned i = 0; i < 0x30; ++i) memory[AramBase + 0x1000 + 2 * i] = 0x10, memory[AramBase + 0x1000 + 2 * i + 1] = 0;

    command(dsp, {0x82011000, Left, Right, 0, 0});  // one frame at output volume 1.0 (4.12)
    check(!dsp.mailWaiting(), "rendering waits for the CPU's first voice sync");
    dsp.send(0), dsp.send(0x00008000);  // group 0: voice 0 only
    check(!dsp.mailWaiting(), "and for the other three groups");
    for (std::uint32_t group : {1u, 2u, 3u}) dsp.send(0), dsp.send(group << 16);
    expect(dsp, {0xDCD10004, 0xF355FF00, 0xDCD10005}, "the frame is acknowledged, then the pass ends");

    check(get16(Left) == 0 && get16(Left + 2 * 3) == 2047 && get16(Left + 2 * 0x32) == 2047 && get16(Left + 2 * 0x33) == 0,
          "the voice's samples reach the left output, after the resampler's three of history");
    check(get16(Right + 2 * 3) == 0, "the right output stays silent");
    check(get16(v + 0x002) == 1, "a voice that ran out of samples is done");
    std::uint32_t remaining;
    std::memcpy(&remaining, at(v + 0x074, 4), 4);
    check(remaining == 0, "its remaining length, a 32-bit field on the CPU's side, reads back whole");

    command(dsp, {0x8E000000, AramBase});
    check(!dsp.mailWaiting(), "commands wait until the task manager answers the frame's end");
    dsp.send(0xCDD10003);
    expect(dsp, {0xDCD10004, 0xF3558E00}, "and run once it does");
    check(!dsp.unsupported(), "nothing asked was beyond this microcode");
}

// AFC: with a zero-coefficient index and scale 1, each 4-bit nibble decodes to itself.
void afc() {
    smg::WiiDsp dsp({at, [] {}});
    dsp.reset();
    dsp.readMail();
    for (std::uint32_t mail : {0x80F3A001u, 0u, 0x80F3C002u, 0u, 0x80F3A002u, 0u, 0x80F3B002u, 0u, 0x80F3D001u, 0u}) dsp.send(mail);
    while (dsp.mailWaiting()) dsp.readMail();
    std::memset(memory.data(), 0, memory.size());
    put32(Tables, 0x00007FFF);
    command(dsp, {0x8E000000, AramBase});
    command(dsp, {0x81000040, Voices, Tables, Afc, Reverb});
    while (dsp.mailWaiting()) dsp.readMail();
    const std::uint32_t v = Voices;
    put16(v + 0x000, 1), put16(v + 0x004, 0x1000), put16(v + 0x008, 1);
    put16(v + 0x010, 0x0D60), put16(v + 0x012, 0x7FFF), put16(v + 0x014, 0x7FFF);
    put16(v + 0x100, 9), put32(v + 0x114, 0x60), put32(v + 0x118, 0x2000);
    // Six blocks of nine bytes: header 0x00, then nibbles 1, 2 ... 7, -8 ... -1, 0, 1 ...
    for (unsigned b = 0; b < 6; ++b) {
        memory[AramBase + 0x2000 + 9 * b] = 0x00;
        for (unsigned i = 0; i < 8; ++i) memory[AramBase + 0x2000 + 9 * b + 1 + i] = static_cast<std::uint8_t>(((2 * i + 1) & 0xF) << 4 | ((2 * i + 2) & 0xF));
    }
    command(dsp, {0x82011000, Left, Right, 0, 0});
    dsp.send(0), dsp.send(0x00008000);
    for (std::uint32_t group : {1u, 2u, 3u}) dsp.send(0), dsp.send(group << 16);
    // Output = nibble * 0x7FFF * 2 >> 16 (the taps), then * 0x7FFF >> 16 (the send), each rounding down: 7 -> 2, -8 -> -4.
    check(get16(Right + 2 * (3 + 6)) == 2 && get16(Right + 2 * (3 + 7)) == -4, "AFC nibbles decode through the voice's coefficients");
    check(!dsp.unsupported(), "nothing asked was beyond this microcode");
}
} // namespace

int runWiiDspTests() {
    protocol();
    afc();
    std::printf("%u Wii DSP checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runWiiDspTests(); }
#endif
