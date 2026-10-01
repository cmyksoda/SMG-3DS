#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <utility>

namespace smg {
// The Wii's audio DSP running JAudio2's own microcode, done in software: the DSP ROM's task boot, the microcode's
// mail protocol and commands, and its frame rendering of up to 64 voices from the blocks the CPU writes (JASDsp's
// TChannel). This is the microcode family Dolphin names "Zelda"; SMG's variant (CRC 0xD643001F) speaks the full
// protocol, reads waves from main memory at a base address in place of ARAM, doubles Dolby volumes, and folds
// command 0D into the render command. Its data are the port's native memory, read as the CPU wrote it.
class WiiDsp {
public:
    struct Host {
        // `size` bytes of the CPU's memory at `address`, or null outside what the port maps.
        std::uint8_t* (*memory)(std::uint32_t address, std::uint32_t size);
        // An interrupting mail has reached the CPU's mailbox.
        void (*interrupt)();
    };
    explicit WiiDsp(const Host& host) : host_(host) {}

    void reset();  // power on: the ROM is waiting, its ready mail in the mailbox
    void send(std::uint32_t mail);  // a mail from the CPU
    bool mailWaiting() const { return !outbox_.empty(); }
    std::uint32_t peekMail() const { return outbox_.empty() ? 0 : outbox_.front().first; }
    std::uint32_t readMail();
    // The first thing the microcode was asked that this doesn't do, for the log; null when none.
    const char* unsupported() const { return unsupported_; }

    using Buffer = std::array<std::int16_t, 0x50>;  // one frame, 2.5 ms at 32 kHz

private:
    enum class State { Rom, Waiting, Rendering, Command, Halted };
    void push(std::uint32_t mail, bool interrupt);
    void romMail(std::uint32_t mail);
    void microcodeMail(std::uint32_t mail);
    void runCommands();
    void acknowledge(bool rendering, std::uint16_t sync);
    void render();
    void note(const char* what) { if (!unsupported_) unsupported_ = what; }

    // Frame rendering, as the microcode mixes.
    struct Voice;
    void prepareFrame();
    void finalizeFrame();
    void addVoice(unsigned id);
    void loadSamples(Voice& voice, Buffer& out);
    void resample(Voice& voice, const std::int16_t* raw, Buffer& out);
    void pcmFromAram(Voice& voice, std::int16_t* out, unsigned count, unsigned bytes);
    void afcFromAram(Voice& voice, std::int16_t* out, unsigned count);
    void decodeAfc(Voice& voice, std::int16_t* out, unsigned blocks);
    void pcmFromMram(Voice& voice, std::int16_t* out, unsigned count);
    void reverb(bool afterRendering);
    Buffer* bufferFor(std::uint16_t id);
    std::int16_t tableHalf(std::uint32_t address, unsigned index, bool words);

    Host host_;
    std::deque<std::pair<std::uint32_t, bool>> outbox_;
    const char* unsupported_ = nullptr;
    char unknownBuffer_[64] = {};
    State state_ = State::Rom;

    // The ROM's boot: which of its parameters the next mail is.
    std::uint32_t romExpect_ = 0, iramAddress_ = 0, iramLength_ = 0;

    // Commands: words in a ring as they arrive, then run when the microcode may.
    std::array<std::uint32_t, 64> commands_{};
    unsigned readAt_ = 0, writeAt_ = 0, pending_ = 0, expected_ = 0;
    bool canRun_ = true;
    std::uint32_t next() {
        const std::uint32_t word = commands_[readAt_];
        readAt_ = (readAt_ + 1) % commands_.size();
        return word;
    }

    // Rendering, and the CPU's permission for each group of 16 voices.
    unsigned framesRequested_ = 0, frame_ = 0, voicesPerFrame_ = 0, voice_ = 0, syncMax_ = 0;
    std::array<std::uint16_t, 4> syncFlags_{};
    bool rendering() const { return frame_ < framesRequested_; }

    // The renderer's tables and buffers.
    bool prepared_ = false;
    std::uint32_t vpbBase_ = 0, reverbBase_ = 0, aramBase_ = 0, outLeft_ = 0, outRight_ = 0;
    std::uint16_t outputVolume_ = 0;
    std::array<std::int16_t, 0x100> resamplingCoeffs_{}, patterns_{};
    std::array<std::int16_t, 0x80> sine_{};
    std::array<std::int16_t, 0x20> afcCoeffs_{};
    Buffer frontLeft_{}, frontRight_{}, backLeft_{}, backRight_{}, frontLeftReverb_{}, frontRightReverb_{}, backLeftReverb_{},
        backRightReverb_{}, unk0Reverb_{}, unk1Reverb_{}, unk0_{}, unk1_{}, unk2_{};
    std::array<std::array<std::int16_t, 8>, 4> reverbLast8_{};
    std::array<std::uint16_t, 4> reverbFrame_{};
};
} // namespace smg
