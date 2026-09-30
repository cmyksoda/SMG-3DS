// The Wii's audio DSP running JAudio2's microcode, done in software (include/smg/WiiDsp.hpp). What the microcode does
// with each field follows Dolphin's study of this family (Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp); it is
// written anew here over the port's memory.
#include <smg/WiiDsp.hpp>
#include <algorithm>
#include <cstring>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "voice blocks are read as a little-endian CPU wrote them");

namespace smg {
namespace {
using s16 = std::int16_t;
using u16 = std::uint16_t;
using s32 = std::int32_t;
using u32 = std::uint32_t;
using s64 = std::int64_t;

// Task mails, both ways.
constexpr u32 DspInit = 0xDCD10000, DspSync = 0xDCD10004, DspFrameEnd = 0xDCD10005;
constexpr u32 MailResume = 0xCDD10000, MailNewMicrocode = 0xCDD10001, MailReset = 0xCDD10002, MailContinue = 0xCDD10003;

s16 clamp16(s64 value) { return static_cast<s16>(std::clamp<s64>(value, -0x8000, 0x7FFF)); }
// The microcode's 32-bit arithmetic wraps; so does this, where C++ would call it undefined.
s32 wrap(s64 value) { return static_cast<s32>(static_cast<u32>(value)); }
s32 shl16(s32 value) { return wrap(s64(value) * 65536); }

// Voice block words, as the microcode numbers them.
enum Word : unsigned {
    Enabled = 0x00, Done = 0x01, ResamplingRatio = 0x02, ResetVpb = 0x04, EndReached = 0x05, UseConstantSample = 0x06,
    Channels = 0x08,  // six of {buffer id, target volume, current volume, unused}
    DolbyPosition = 0x28, DolbyReverb = 0x29, DolbyCurrent = 0x2A, DolbyTarget = 0x2B, UseDolby = 0x2C,
    PosFrac = 0x30, AfcRemainingDecoded = 0x32, ConstantSample = 0x33, Position = 0x34, SamplesBeforeLoop = 0x36,
    AramAddress = 0x38, RemainingLength = 0x3A, ResampleBuffer = 0x3C, BiquadHistory = 0x54, AfcSamples = 0x58,
    LowPassYn1 = 0x68, LowPassXn1 = 0x69,
    SourceType = 0x80, Looping = 0x81, LoopYn1 = 0x82, LoopYn2 = 0x83, FilterFlags = 0x84, EndRequested = 0x85,
    LoopAddress = 0x88, LoopStart = 0x8A, BaseAddress = 0x8C, BiquadCoeffs = 0xA4, LowPassCoeff = 0xA8,
};
// JAudio2's CPU writes these pairs as one 32-bit field (TChannel's _68, _70, _74, _10C, _110, _114, _118, _11C).
bool cpuWordPair(unsigned first) {
    return first == 0x34 || first == 0x38 || first == 0x3A || first == 0x86 || first == 0x88 || first == 0x8A || first == 0x8C ||
           first == 0x8E;
}
// Where a microcode word sits in the CPU's block: in place, or the other half of a 32-bit field on this CPU.
unsigned byteOf(unsigned word) {
    if (cpuWordPair(word)) return 2 * word + 2;
    if (word > 0 && cpuWordPair(word - 1)) return 2 * word - 2;
    return 2 * word;
}

// Sample sources.
enum Source : u16 {
    SquareWave = 0, SawWave = 1, SquareWave25 = 3, Pattern1 = 4, AfcLow = 5, Pattern0 = 7, Pcm8Aram = 8, AfcHigh = 9,
    Pattern0Variable = 10, Pattern2 = 11, Pattern3 = 12, Pcm16Aram = 16, Pcm16Mram = 33,
};

s32 addWithRamp(WiiDsp::Buffer& dst, const WiiDsp::Buffer& src, s32 volume, s32 step) {
    if (!volume && !step) return volume;
    for (std::size_t i = 0; i < dst.size(); ++i) {
        dst[i] = static_cast<s16>(dst[i] + (((volume >> 16) * src[i]) >> 16));
        volume = wrap(s64(volume) + step);
    }
    return volume;
}
void addWithVolume(s16* dst, const s16* src, std::size_t count, u16 volume) {
    while (count--) {
        const s32 scaled = (static_cast<s32>(*src++) * static_cast<s16>(volume)) >> 15;
        *dst = static_cast<s16>(*dst + clamp16(scaled));
        ++dst;
    }
}
template <int IntegerBits>
void applyVolume(WiiDsp::Buffer& buffer, u16 volume) {
    for (s16& sample : buffer) sample = clamp16((static_cast<s32>(sample) * volume) >> (16 - IntegerBits));
}
} // namespace

// A voice block in the microcode's word order, fetched from and stored back to the CPU's TChannel.
struct WiiDsp::Voice {
    u16 w[0xC0];
    u32 get32(unsigned at) const { return u32(w[at]) << 16 | w[at + 1]; }
    void set32(unsigned at, u32 value) { w[at] = static_cast<u16>(value >> 16), w[at + 1] = static_cast<u16>(value); }
    s16 s(unsigned at) const { return static_cast<s16>(w[at]); }
};

void WiiDsp::reset() {
    *this = WiiDsp(host_);
    push(0x8071FEED, false);  // the ROM's "ready" mail
}

void WiiDsp::push(u32 mail, bool interrupt) {
    outbox_.emplace_back(mail, interrupt);
    if (interrupt && outbox_.size() == 1) host_.interrupt();
}

u32 WiiDsp::readMail() {
    if (outbox_.empty()) return 0;
    const u32 mail = outbox_.front().first;
    outbox_.pop_front();
    // The next interrupting mail interrupts as it reaches the mailbox.
    if (!outbox_.empty() && outbox_.front().second) host_.interrupt();
    return mail;
}

void WiiDsp::send(u32 mail) {
    if (state_ == State::Rom) romMail(mail);
    else microcodeMail(mail);
}

// The ROM's task boot (dsp_task.c's __DSP_boot_task): addresses and lengths, then the start vector boots the task.
void WiiDsp::romMail(u32 mail) {
    if (romExpect_) {
        if (romExpect_ == 0x80F3A001) iramAddress_ = mail;
        if (romExpect_ == 0x80F3A002) iramLength_ = mail;
        const bool boot = romExpect_ == 0x80F3D001;
        romExpect_ = 0;
        if (boot) {
            // JAudio2's task is its own microcode; its init mail, then its handshake.
            state_ = State::Waiting;
            push(DspInit, true);
            push(0xF3551111, false);
        }
        return;
    }
    if ((mail & 0xFFFF0000) == 0x80F30000) romExpect_ = mail;
    else note("a mail to the DSP ROM outside its task boot");
}

void WiiDsp::microcodeMail(u32 mail) {
    switch (state_) {
    case State::Waiting:
        if (mail & 0x80000000) {
            // A task mail ends a rendering pass (the task manager's answer to DSP_FRAME_END).
            if ((mail & 0xFFFF0000) != 0xCDD10000) mail = 0xCDD10000 | (mail & 0xFFFF);
            if (mail == MailContinue) {
                canRun_ = true;
                runCommands();
            } else if (mail == MailNewMicrocode) {
                note("switching the DSP to another microcode");
                state_ = State::Halted;
            } else {
                if (mail != MailResume && mail != MailReset) note("an unknown task mail");
                state_ = State::Halted;
            }
        } else if (!(mail & 0xFFFF)) {
            // A voice sync: the next mail says which voices of a group of 16 may render.
            if (rendering()) state_ = State::Rendering;
            else {
                note("a voice sync while nothing renders");
                state_ = State::Halted;
            }
        } else {
            state_ = State::Command;
            expected_ = mail & 0xFFFF;
        }
        break;
    case State::Rendering:
        syncMax_ = (((mail >> 16) & 0xF) + 1) << 4;
        syncFlags_[(mail >> 16) & 3] = static_cast<u16>(mail);
        state_ = State::Waiting;
        render();
        break;
    case State::Command:
        commands_[writeAt_] = mail;
        writeAt_ = (writeAt_ + 1) % commands_.size();
        if (--expected_ == 0) {
            ++pending_;
            state_ = State::Waiting;
            runCommands();
        }
        break;
    case State::Rom:
    case State::Halted:
        break;
    }
}

void WiiDsp::acknowledge(bool rendering, u16 sync) {
    push(rendering ? DspFrameEnd : DspSync, true);
    if (!rendering) push(0xF3550000 | sync, false);
}

// Commands run one after another, except while rendering or before the task manager lets a pass end.
void WiiDsp::runCommands() {
    while (pending_ && !rendering() && canRun_) {
        const u32 mail = next();
        if (!(mail & 0x80000000)) continue;
        --pending_;
        const u32 command = (mail >> 24) & 0x7F;
        const u16 sync = static_cast<u16>(mail >> 16), extra = static_cast<u16>(mail);
        switch (command) {
        case 0x00: case 0x03: case 0x0A: case 0x0B: case 0x0C: case 0x0F:
            acknowledge(false, sync);
            break;
        case 0x01: {  // setup: voice blocks, the coefficient tables, reverb blocks
            voicesPerFrame_ = extra;
            vpbBase_ = next();
            const u32 tables = next();
            // JASDsp::DSPRES_FILTER, declared as 32-bit words: resampling taps, then patterns, then the sine.
            for (unsigned i = 0; i < 0x100; ++i) resamplingCoeffs_[i] = tableHalf(tables, i, true);
            for (unsigned i = 0; i < 0x100; ++i) patterns_[i] = tableHalf(tables + 0x200, i, true);
            for (unsigned i = 0; i < 0x80; ++i) sine_[i] = tableHalf(tables + 0x400, i, true);
            // JASDsp::DSPADPCM_FILTER, declared as bytes.
            const u32 afc = next();
            for (unsigned i = 0; i < 0x20; ++i) afcCoeffs_[i] = tableHalf(afc, i, false);
            reverbBase_ = next();
            acknowledge(false, sync);
            break;
        }
        case 0x02:  // render frames into two output buffers; command 0D's two words ride along
            framesRequested_ = (mail >> 16) & 0xFF;
            outputVolume_ = extra;
            outLeft_ = next(), outRight_ = next();
            next(), next();
            frame_ = voice_ = 0;
            render();
            return;
        case 0x0D:
            next();
            acknowledge(false, sync);
            break;
        case 0x0E:  // where "ARAM" addresses start in main memory
            aramBase_ = next();
            acknowledge(false, sync);
            break;
        default:
            note("an unknown microcode command");
            state_ = State::Halted;
            return;
        }
    }
}

std::int16_t WiiDsp::tableHalf(u32 address, unsigned index, bool words) {
    const std::uint8_t* at = host_.memory(address + (index & ~1u) * 2, 4);
    if (!at) return 0;
    if (words) {
        u32 word;
        std::memcpy(&word, at, 4);
        return static_cast<s16>(index & 1 ? word : word >> 16);
    }
    return static_cast<s16>(at[(index & 1) * 2] << 8 | at[(index & 1) * 2 + 1]);
}

void WiiDsp::render() {
    while (frame_ < framesRequested_) {
        if (voice_ == 0) prepareFrame();
        const unsigned voices = std::min<unsigned>(voicesPerFrame_, syncFlags_.size() << 4);
        while (voice_ < voices) {
            if (voice_ >= syncMax_) return;  // wait for the CPU's next voice sync
            if (syncFlags_[voice_ >> 4] & (1u << (0xF - (voice_ & 0xF)))) addVoice(voice_);
            ++voice_;
        }
        acknowledge(false, static_cast<u16>(0xFF00 | frame_));
        finalizeFrame();
        voice_ = syncMax_ = 0;
        ++frame_;
    }
    acknowledge(true, 0);
    canRun_ = false;  // until the task manager answers the frame's end
}

void WiiDsp::prepareFrame() {
    if (prepared_) return;
    frontLeft_.fill(0), frontRight_.fill(0);
    applyVolume<1>(backLeft_, 0x6784);
    applyVolume<1>(backRight_, 0x6784);
    reverb(false);
    addWithVolume(frontLeftReverb_.data(), backLeftReverb_.data(), 0x50, 0x7FFF);
    addWithVolume(frontRightReverb_.data(), backLeftReverb_.data(), 0x50, 0xB820);
    addWithVolume(frontLeftReverb_.data(), backRightReverb_.data() + 0x28, 0x28, 0xB820);
    addWithVolume(frontRightReverb_.data(), backRightReverb_.data() + 0x28, 0x28, 0x7FFF);
    backLeftReverb_.fill(0), backRightReverb_.fill(0);
    // Patterns 2 and 3 change every frame.
    s16* pattern2 = patterns_.data() + 0x80;
    s32 yn2 = pattern2[0x3E], yn1 = pattern2[0x3F];
    for (int i = 0; i < 0x40; i += 2) {
        s32 v = wrap(s64(yn2) * yn1 - s64(shl16(pattern2[i])));
        yn2 = yn1, yn1 = pattern2[i], pattern2[i] = static_cast<s16>(v >> 16);
        v = wrap(2 * (s64(yn2) * yn1 + shl16(pattern2[i + 1])));
        yn2 = yn1, yn1 = pattern2[i + 1], pattern2[i + 1] = static_cast<s16>(v >> 16);
    }
    s16* pattern3 = patterns_.data() + 0xC0;
    yn2 = pattern3[0x3E], yn1 = pattern3[0x3F];
    const s16 start = static_cast<s16>(yn1);
    s16 step = static_cast<s16>(pattern3[0] + (wrap(s64(yn1) * yn2 + wrap(s64(shl16(yn2)) + yn1)) >> 16));
    step = static_cast<s16>((step & 0x1FF) | 0x2000);
    for (s32 i = 0; i < 0x40; ++i) pattern3[i] = static_cast<s16>(start + (i + 1) * step);
    prepared_ = true;
}

void WiiDsp::finalizeFrame() {
    applyVolume<4>(frontLeft_, outputVolume_);
    applyVolume<4>(frontRight_, outputVolume_);
    for (auto [address, buffer] : {std::pair{&outLeft_, &frontLeft_}, std::pair{&outRight_, &frontRight_}}) {
        if (std::uint8_t* out = host_.memory(*address, sizeof(Buffer))) std::memcpy(out, buffer->data(), sizeof(Buffer));
        *address += sizeof(Buffer);
    }
    reverb(true);
    prepared_ = false;
}

// A mixing destination by its address in the DSP's memory. The sends JAudio2 names 8 samples in (0x0DC8, 0x0E28,
// 0x0E88, 0x0EE8) are the reverb buffers past their 8 samples of filter history.
WiiDsp::Buffer* WiiDsp::bufferFor(u16 id) {
    switch (id) {
    case 0x0D00: return &frontLeft_;
    case 0x0D60: return &frontRight_;
    case 0x0F40: return &backLeft_;
    case 0x0CA0: return &backRight_;
    case 0x0E80: case 0x0E88: return &frontLeftReverb_;
    case 0x0EE0: case 0x0EE8: return &frontRightReverb_;
    case 0x0C00: return &backLeftReverb_;
    case 0x0C50: return &backRightReverb_;
    case 0x0DC0: case 0x0DC8: return &unk0Reverb_;
    case 0x0E20: case 0x0E28: return &unk1Reverb_;
    case 0x09A0: return &unk0_;
    case 0x0FA0: return &unk1_;
    case 0x0B00: return &unk2_;
    default: return nullptr;
    }
}

// The four reverb blocks (JASDsp::FxBuf): a circular buffer of frames in main memory, filtered and sent onward.
void WiiDsp::reverb(bool afterRendering) {
    if (!reverbBase_) return;
    Buffer* buffers[4] = {&unk0Reverb_, &unk1Reverb_, &frontLeftReverb_, &frontRightReverb_};
    for (unsigned r = 0; r < 4; ++r) {
        const std::uint8_t* block = host_.memory(reverbBase_ + r * 0x20, 0x20);
        if (!block) continue;
        u16 enabled, size, dest[4];
        u32 base;
        s16 coeffs[8];
        std::memcpy(&enabled, block, 2), std::memcpy(&size, block + 2, 2), std::memcpy(&base, block + 4, 4);
        std::memcpy(dest, block + 8, 8), std::memcpy(coeffs, block + 0x10, 16);
        if (!enabled || !size) continue;
        const u32 address = base + reverbFrame_[r] * sizeof(Buffer);
        if (!afterRendering) {
            s16 samples[0x58];
            std::memcpy(samples, reverbLast8_[r].data(), 16);
            if (const std::uint8_t* from = host_.memory(address, sizeof(Buffer))) std::memcpy(samples + 8, from, sizeof(Buffer));
            else std::memset(samples + 8, 0, sizeof(Buffer));
            std::memcpy(reverbLast8_[r].data(), samples + 0x50, 16);
            const auto filter = [&] {
                for (unsigned i = 0; i < 0x50; ++i) {
                    s32 sum = 0;
                    for (unsigned j = 0; j < 8; ++j) sum += s32(samples[i + j]) * coeffs[j];
                    samples[i] = clamp16(sum >> 15);
                }
            };
            if (enabled & 1) filter();
            for (unsigned d = 0; d < 2; ++d)
                if (Buffer* to = dest[2 * d] ? bufferFor(dest[2 * d]) : nullptr) addWithVolume(to->data(), samples, 0x50, dest[2 * d + 1]);
            if (enabled & 2) filter();
            std::memcpy(buffers[r]->data(), samples, sizeof(Buffer));
        } else {
            if (std::uint8_t* to = host_.memory(address, sizeof(Buffer))) std::memcpy(to, buffers[r]->data(), sizeof(Buffer));
            reverbFrame_[r] = static_cast<u16>((reverbFrame_[r] + 1) % size);
        }
    }
}

void WiiDsp::addVoice(unsigned id) {
    std::uint8_t* block = host_.memory(vpbBase_ + id * 0x180, 0x180);
    if (!block) return;
    Voice v;
    for (unsigned word = 0; word < 0xC0; ++word) std::memcpy(&v.w[word], block + byteOf(word), 2);
    if (!v.w[Enabled] || v.w[Done]) return;

    Buffer input;
    loadSamples(v, input);
    if (const u16 coeff = v.w[LowPassCoeff]) {
        s32 yn1 = v.w[ResetVpb] ? 0 : v.s(LowPassYn1), xn1 = v.w[ResetVpb] ? 0 : v.s(LowPassXn1);
        for (s16& sample : input) {
            const s32 xn0 = sample;
            const s16 yn0 = clamp16((s64(xn0 - xn1) * coeff >> 7) + yn1);
            sample = yn0, yn1 = yn0, xn1 = xn0;
        }
        v.w[LowPassYn1] = static_cast<u16>(yn1), v.w[LowPassXn1] = static_cast<u16>(xn1);
    }
    if (v.w[FilterFlags] & 0x1F) note("a voice's variable FIR filter");
    const s16 bn1 = v.s(BiquadCoeffs), bn2 = v.s(BiquadCoeffs + 1), an1 = v.s(BiquadCoeffs + 2), an2 = v.s(BiquadCoeffs + 3);
    if ((v.w[FilterFlags] & 0x20) && (an2 || an1 || bn2 || bn1 != 0x7FFF)) {
        s32 xn1 = v.s(BiquadHistory), xn2 = v.s(BiquadHistory + 1), yn1 = v.s(BiquadHistory + 2), yn2 = v.s(BiquadHistory + 3);
        for (s16& sample : input) {
            const s32 xn0 = sample;
            const s16 yn0 = clamp16((s64(bn1) * xn1 + s64(bn2) * xn2 + s64(an1) * yn1 + s64(an2) * yn2) >> 15);
            sample = yn0, xn2 = xn1, xn1 = xn0, yn2 = yn1, yn1 = yn0;
        }
        v.w[BiquadHistory] = static_cast<u16>(xn1), v.w[BiquadHistory + 1] = static_cast<u16>(xn2);
        v.w[BiquadHistory + 2] = static_cast<u16>(yn1), v.w[BiquadHistory + 3] = static_cast<u16>(yn2);
    }

    if (v.w[UseDolby]) {
        if (v.w[EndRequested]) {
            v.w[DolbyTarget] = static_cast<u16>(v.s(DolbyCurrent) / 2);
            if (!v.w[DolbyTarget]) v.w[Done] = 1;
        }
        const unsigned x = (v.w[DolbyPosition] >> 8) & 0x7F, y = v.w[DolbyPosition] & 0x7F;
        const s16 right = sine_[x], back = sine_[y], left = sine_[x ^ 0x7F], front = sine_[y ^ 0x7F];
        const int shift = 15;  // SMG's microcode doubles Dolby volumes
        s16 quadrant[4] = {static_cast<s16>((left * front) >> shift), static_cast<s16>((left * back) >> shift),
                           static_cast<s16>((right * front) >> shift), static_cast<s16>((right * back) >> shift)};
        const s16 delta = static_cast<s16>(v.s(DolbyTarget) - v.s(DolbyCurrent));
        s16 deltas[4], reverbVolumes[4], reverbDeltas[4];
        for (int i = 0; i < 4; ++i) deltas[i] = static_cast<s16>((static_cast<u16>(quadrant[i]) * delta) >> shift);
        for (s16& q : quadrant) q = static_cast<s16>((q * v.s(DolbyCurrent)) >> shift);
        for (int i = 0; i < 4; ++i) {
            reverbVolumes[i] = static_cast<s16>((quadrant[i] * v.s(DolbyReverb)) >> shift);
            reverbDeltas[i] = static_cast<s16>((deltas[i] * v.s(DolbyReverb)) >> shift);
        }
        const struct { Buffer* to; s16 volume, delta; } sends[8] = {
            {&frontLeft_, quadrant[0], deltas[0]}, {&backLeft_, quadrant[1], deltas[1]},
            {&frontRight_, quadrant[2], deltas[2]}, {&backRight_, quadrant[3], deltas[3]},
            {&frontLeftReverb_, reverbVolumes[0], reverbDeltas[0]}, {&backLeftReverb_, reverbVolumes[1], reverbDeltas[1]},
            {&frontRightReverb_, reverbVolumes[2], reverbDeltas[2]}, {&backRightReverb_, reverbVolumes[3], reverbDeltas[3]}};
        for (const auto& send : sends) addWithRamp(*send.to, input, shl16(send.volume), shl16(send.delta) / s32(input.size()));
        v.w[DolbyCurrent] = v.w[DolbyTarget];
    } else {
        if (v.w[EndRequested]) {
            bool silent = true;
            for (int c = 0; c < 6; ++c) {
                v.w[Channels + 4 * c + 1] = static_cast<u16>(v.s(Channels + 4 * c + 2) / 2);
                silent &= v.w[Channels + 4 * c + 1] == 0;
            }
            if (silent) v.w[Done] = 1;
        }
        for (int c = 0; c < 6; ++c) {
            const u16 destination = v.w[Channels + 4 * c];
            if (!destination) continue;
            const s16 target = v.s(Channels + 4 * c + 1), current = v.s(Channels + 4 * c + 2);
            const s32 step = shl16(static_cast<s16>(target - current)) / s32(input.size());
            if (!current && !step) continue;
            if (Buffer* to = bufferFor(destination)) {
                v.w[Channels + 4 * c + 2] = static_cast<u16>(addWithRamp(*to, input, shl16(current), step) >> 16);
            } else {
                note("a voice sent to a mixing buffer the port doesn't know");
            }
        }
    }
    if (!v.w[UseConstantSample]) v.w[ResetVpb] = 0;
    // Only the first 0x80 words go back: the rest is the CPU's.
    for (unsigned word = 0; word < 0x80; ++word) std::memcpy(block + byteOf(word), &v.w[word], 2);
}

void WiiDsp::loadSamples(Voice& v, Buffer& out) {
    // Up to 0x500 raw samples at the highest ratio, 16 more for AFC's rounding, after the last frame's four.
    s16 raw[4 + 0x500 + 0x10];
    for (int i = 0; i < 4; ++i) raw[i] = v.s(ResampleBuffer + i);
    if (v.w[UseConstantSample]) {
        out.fill(v.s(ConstantSample));
        return;
    }
    const u16 source = v.w[SourceType];
    const unsigned needed = (v.w[PosFrac] + 0x50u * v.w[ResamplingRatio]) >> 12;
    switch (source) {
    case SquareWave: case SquareWave25: {
        const u32 shift = source == SquareWave ? 1 : 2, mask = (1u << shift) - 1, ratio = u32(v.w[ResamplingRatio]) << (shift - 1);
        u32 pos = u32(v.w[PosFrac]) << shift;
        for (s16& sample : out) sample = static_cast<s16>(((pos >> 16) & mask) ? 0xC000 : 0x4000), pos += ratio;
        v.w[PosFrac] = static_cast<u16>(pos >> shift);
        break;
    }
    case SawWave: {
        u32 pos = v.w[PosFrac];
        for (s16& sample : out) sample = static_cast<s16>(pos), pos += v.w[ResamplingRatio] >> 1;
        v.w[PosFrac] = static_cast<u16>(pos);
        break;
    }
    case Pattern0: case Pattern0Variable: case Pattern1: case Pattern2: case Pattern3: {
        const unsigned index = source == Pattern1 ? 1 : source == Pattern2 ? 2 : source == Pattern3 ? 3 : 0;
        const s16* pattern = patterns_.data() + index * 0x40;
        u32 pos = u32(v.w[PosFrac]) << 6;
        const u32 step = u32(v.w[ResamplingRatio]) << 5;
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = pattern[pos >> 16];
            pos = (pos + step) % (0x40u << 16);
            if (source == Pattern0Variable) pos = static_cast<u32>((s64(pos) * 1024 + s64(backRight_[i]) * v.w[ResamplingRatio]) >> 10);
        }
        v.w[PosFrac] = static_cast<u16>(pos >> 6);
        break;
    }
    case Pcm8Aram: pcmFromAram(v, raw + 4, needed, 1), resample(v, raw, out); break;
    case Pcm16Aram: pcmFromAram(v, raw + 4, needed, 2), resample(v, raw, out); break;
    case AfcHigh: case AfcLow: afcFromAram(v, raw + 4, needed), resample(v, raw, out); break;
    case Pcm16Mram: pcmFromMram(v, raw + 4, needed), resample(v, raw, out); break;
    default:
        note("a voice sample source the port doesn't know");
        out.fill(0);
    }
}

void WiiDsp::resample(Voice& v, const s16* raw, Buffer& out) {
    const u32 ratio = v.w[ResamplingRatio];
    u32 pos = v.w[PosFrac];
    if ((ratio >> 12) >= 4) {
        for (s16& sample : out) pos += ratio, sample = raw[pos >> 12];
    } else {
        for (s16& sample : out) {
            const s16* taps = &resamplingCoeffs_[((pos & 0xFFF) >> 6) * 4];
            const s16* in = &raw[pos >> 12];
            s64 sum = 0;
            for (int i = 0; i < 4; ++i) sum += s64(2) * taps[i] * in[i];
            sample = clamp16(sum >> 16);
            pos += ratio;
        }
    }
    for (int i = 0; i < 4; ++i) v.w[ResampleBuffer + i] = static_cast<u16>(raw[(pos >> 12) + i]);
    v.w[ConstantSample] = static_cast<u16>(out.back());
    v.w[PosFrac] = static_cast<u16>(pos & 0xFFF);
}

// PCM from "ARAM" (main memory past the base the CPU gave): big-endian samples, as on the disc.
void WiiDsp::pcmFromAram(Voice& v, s16* out, unsigned count, unsigned bytes) {
    if (v.w[Done]) {
        std::fill(out, out + count, 0);
        return;
    }
    if (v.w[ResetVpb]) {
        v.set32(RemainingLength, v.get32(LoopStart) - v.get32(Position));
        v.set32(AramAddress, v.get32(BaseAddress) + v.get32(Position) * bytes);
    }
    v.w[EndReached] = 0;
    while (count) {
        if (v.w[EndReached]) {
            v.w[EndReached] = 0;
            if (!v.w[Looping]) {
                std::fill(out, out + count, 0);
                v.w[Done] = 1;
                break;
            }
            v.set32(Position, v.get32(LoopAddress));
            v.set32(RemainingLength, v.get32(LoopStart) - v.get32(Position));
            v.set32(AramAddress, v.get32(BaseAddress) + v.get32(Position) * bytes);
        }
        const unsigned batch = std::min<u32>(v.get32(RemainingLength), count);
        const std::uint8_t* from = host_.memory(aramBase_ + v.get32(AramAddress), batch * bytes);
        for (unsigned i = 0; i < batch; ++i)
            *out++ = !from ? 0 : bytes == 1 ? static_cast<s16>(from[i] << 8) : static_cast<s16>(from[2 * i] << 8 | from[2 * i + 1]);
        v.set32(RemainingLength, v.get32(RemainingLength) - batch);
        v.set32(AramAddress, v.get32(AramAddress) + batch * bytes);
        count -= batch;
        if (!v.get32(RemainingLength)) v.w[EndReached] = 1;
    }
}

void WiiDsp::afcFromAram(Voice& v, s16* out, unsigned count) {
    s16* history = reinterpret_cast<s16*>(&v.w[AfcSamples]);
    if (v.w[ResetVpb]) {
        history[0xE] = history[0xF] = 0;
        v.w[AfcRemainingDecoded] = 0;
        v.set32(RemainingLength, v.get32(LoopStart));
        v.set32(AramAddress, v.get32(BaseAddress));
    }
    if (v.w[Done]) {
        std::fill(out, out + count, 0);
        return;
    }
    v.w[AfcRemainingDecoded] = std::min<u16>(v.w[AfcRemainingDecoded], 0x10);
    while (true) {
        const unsigned cached = std::min<unsigned>(v.w[AfcRemainingDecoded], count);
        const s16* from = &history[0x10 - v.w[AfcRemainingDecoded]];
        for (unsigned i = 0; i < cached; ++i) *out++ = from[i];
        v.w[AfcRemainingDecoded] = static_cast<u16>(v.w[AfcRemainingDecoded] - cached);
        count -= cached;
        if (!count) return;
        if (count <= v.get32(RemainingLength)) {
            const unsigned blocks = (count + 0xF) >> 4, decoded = blocks << 4;
            if (decoded < v.get32(RemainingLength)) {
                v.w[AfcRemainingDecoded] = static_cast<u16>(decoded - count);
                v.set32(RemainingLength, v.get32(RemainingLength) - decoded);
            } else {
                v.w[AfcRemainingDecoded] = static_cast<u16>(v.get32(RemainingLength) - count);
                v.set32(RemainingLength, 0);
            }
            decodeAfc(v, out, blocks);
            if (v.w[AfcRemainingDecoded]) {
                for (unsigned i = 0; i < 0x10; ++i) history[i] = out[decoded - 0x10 + i];
                if (!v.get32(RemainingLength) && v.get32(LoopStart)) {
                    // Line the kept samples up with the loop that comes next.
                    const s16* base = history + ((v.get32(LoopStart) + 0xF) & 0xF);
                    for (unsigned i = 0; i < v.w[AfcRemainingDecoded]; ++i) history[0x10 - i - 1] = *base--;
                }
            }
            return;
        }
        if (const unsigned left = v.get32(RemainingLength)) {
            count -= left;
            decodeAfc(v, out, (left + 0xF) >> 4);
            out += left;
        }
        if (!v.w[Looping]) {
            v.w[Done] = 1;
            std::fill(out, out + count, 0);
            return;
        }
        // Loop: the source number is also the bytes per 16 samples.
        v.set32(AramAddress, v.get32(BaseAddress) + (v.get32(LoopAddress) >> 4) * v.w[SourceType]);
        history[0xE] = v.s(LoopYn2), history[0xF] = v.s(LoopYn1);
        decodeAfc(v, history, 1);
        v.w[AfcRemainingDecoded] = static_cast<u16>(0x10 - (v.get32(LoopAddress) & 0xF));
        v.set32(RemainingLength, v.get32(LoopStart) - v.w[AfcRemainingDecoded] - v.get32(LoopAddress));
    }
}

void WiiDsp::decodeAfc(Voice& v, s16* out, unsigned blocks) {
    s16* history = reinterpret_cast<s16*>(&v.w[AfcSamples]);
    const unsigned blockBytes = v.w[SourceType];
    const u32 address = v.get32(AramAddress);
    const std::uint8_t* src = host_.memory(aramBase_ + address, blocks * blockBytes);
    v.set32(AramAddress, address + blocks * blockBytes);
    if (!src) {
        std::fill(out, out + blocks * 16, 0);
        return;
    }
    for (unsigned b = 0; b < blocks; ++b) {
        s16 nibbles[16];
        const s16 delta = static_cast<s16>(1 << ((*src >> 4) & 0xF));
        const unsigned index = *src++ & 0xF;
        if (blockBytes == AfcHigh) {
            for (int i = 0; i < 16; i += 2, ++src) nibbles[i] = static_cast<s16>(*src >> 4), nibbles[i + 1] = static_cast<s16>(*src & 0xF);
            for (s16& n : nibbles) n = static_cast<s16>(static_cast<s16>(static_cast<u16>(n << 12)) >> 1);
        } else {
            for (int i = 0; i < 16; i += 4, ++src)
                for (int k = 0; k < 4; ++k) nibbles[i + k] = static_cast<s16>((*src >> (6 - 2 * k)) & 3);
            for (s16& n : nibbles) n = static_cast<s16>(static_cast<s16>(static_cast<u16>(n << 14)) >> 1);
        }
        s32 yn1 = history[0xF], yn2 = history[0xE];
        for (s16 n : nibbles) {
            const s32 sample = std::clamp<s64>((s64(delta) * n + s64(yn1) * afcCoeffs_[index * 2] + s64(yn2) * afcCoeffs_[index * 2 + 1]) >> 11, -0x8000, 0x7FFF);
            *out++ = static_cast<s16>(sample);
            yn2 = yn1, yn1 = sample;
        }
        history[0xE] = static_cast<s16>(yn2), history[0xF] = static_cast<s16>(yn1);
    }
}

// PCM16 straight from main memory, its position in the first half of the position pair.
void WiiDsp::pcmFromMram(Voice& v, s16* out, unsigned count) {
    const auto copy = [&](s16* to, u32 address, unsigned samples) {
        const std::uint8_t* from = host_.memory(address, samples * 2);
        for (unsigned i = 0; i < samples; ++i) to[i] = from ? static_cast<s16>(from[2 * i] << 8 | from[2 * i + 1]) : 0;
    };
    const u32 address = v.get32(BaseAddress) + v.w[Position] * 2u, remaining = v.get32(RemainingLength);
    if (count > remaining) {
        s16 last = 0;
        if (remaining) copy(out, address, remaining), last = out[remaining - 1];
        std::fill(out + remaining, out + count, last);
        v.w[Position] = static_cast<u16>(v.w[Position] + remaining);
        v.set32(RemainingLength, 0);
        v.w[Done] = 1;
        return;
    }
    v.set32(RemainingLength, remaining - count);
    v.w[SamplesBeforeLoop] = static_cast<u16>(v.w[LoopStart] - v.w[Position]);
    if (count <= v.w[SamplesBeforeLoop]) {
        copy(out, address, count);
        v.w[Position] = static_cast<u16>(v.w[Position] + count);
    } else {
        copy(out, address, v.w[SamplesBeforeLoop]);
        v.set32(BaseAddress, v.get32(LoopAddress));
        v.w[Position] = static_cast<u16>(count - v.w[SamplesBeforeLoop]);
        copy(out + v.w[SamplesBeforeLoop], v.get32(LoopAddress), v.w[Position]);
    }
}
} // namespace smg
