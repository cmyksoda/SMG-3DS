// The Wii's audio hardware as the SDK's DSP and AI libraries saw it. The DSP runs JAudio2's microcode in software
// (smg::WiiDsp) behind its mailboxes; the AI's DMA plays each buffer the game hands it through the 3DS's own DSP
// (NDSP) and interrupts as each one starts, so JAudio2's audio thread paces itself as on the Wii.
#include <3ds.h>
#include <smg/FreeStore.hpp>
#include <smg/WiiDsp.hpp>
#include <revolution/ai.h>
#include <revolution/dsp.h>
#include <revolution/os.h>
#include <revolution/os/OSInterrupt.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt);
extern "C" void __DSPHandler(__OSInterrupt interrupt, OSContext* context);  // JAudio2's (osdsp_task.cpp)

namespace {
// The DSP block's registers, which JAudio2's interrupt handler acknowledges by writing; the mailboxes are the
// functions below.
alignas(4) volatile u16 sDspRegisters[0x40];

// Addresses are the port's own (the Wii's cached, uncached and physical views are one address space here).
std::uint8_t* memory(std::uint32_t address, std::uint32_t) { return address ? reinterpret_cast<std::uint8_t*>(address) : nullptr; }

bool sRaiseDsp = false;
smg::WiiDsp sDsp({memory, [] { sRaiseDsp = true; }});
RecursiveLock sDspLock;

// Each call finishes with the DSP, then raises what it asked for, so JAudio2's handler never runs inside it.
template <typename Call>
auto withDsp(Call call) {
    const smg::PortMemory memory;
    RecursiveLock_Lock(&sDspLock);
    auto result = call();
    const bool raise = sRaiseDsp;
    sRaiseDsp = false;
    if (const char* what = sDsp.unsupported()) {
        static const char* said = nullptr;
        if (what != said) std::fprintf(stderr, "The Wii's DSP microcode: %s is not the port's yet\n", what), said = what;
    }
    RecursiveLock_Unlock(&sDspLock);
    if (raise) smgRaiseInterrupt(__OS_INTERRUPT_DSP_DSP);
    return result;
}

// The AI: the buffer the game last handed over, and the engine playing them one after another.
constexpr int WaveBuffers = 3;
constexpr std::size_t MaxBuffer = 0x4000;
AIDCallback sDmaCallback = nullptr;
volatile u32 sDmaStart = 0, sDmaLength = 0;
u32 sSampleRate = 32000;
volatile bool sPlaying = false;
Thread sEngine = nullptr;
LightEvent sNdspFrame;
bool sNdsp = false;
ndspWaveBuf sWaves[WaveBuffers];
s16* sWaveMemory = nullptr;

void aiInterrupt(__OSInterrupt, OSContext*) {
    if (sDmaCallback) sDmaCallback();
}

// Copies the buffer the game handed over into the next wave, as the DMA took it, and tells the game it began.
void startBlock(ndspWaveBuf* wave) {
    const u32 length = std::min<u32>(u32(sDmaLength), MaxBuffer);
    if (wave) {
        s16* samples = const_cast<s16*>(wave->data_pcm16);
        if (sDmaStart && length) std::memcpy(samples, reinterpret_cast<const void*>(static_cast<uintptr_t>(sDmaStart)), length);
        wave->nsamples = length / 4;
        DSP_FlushDataCache(samples, length);
        ndspChnWaveBufAdd(0, wave);
    }
    smgRaiseInterrupt(__OS_INTERRUPT_DSP_AI);
}

void engine(void*) {
    int next = 0;
    while (true) {
        if (!sPlaying) {
            svcSleepThread(1000000);
            continue;
        }
        if (sNdsp) {
            if (sWaves[next].status == NDSP_WBUF_DONE || sWaves[next].status == NDSP_WBUF_FREE) {
                startBlock(&sWaves[next]);
                next = (next + 1) % WaveBuffers;
            } else {
                LightEvent_Wait(&sNdspFrame);
            }
        } else {
            // No output: blocks still take their time, so the game's audio keeps its pace.
            const u32 frames = std::max<u32>(u32(sDmaLength) / 4, 1);
            startBlock(nullptr);
            svcSleepThread(static_cast<s64>(frames) * 1000000000LL / sSampleRate);
        }
    }
}
} // namespace

extern "C" {
volatile void* smgAudioRegisters() { return sDspRegisters; }

// DSP (the SDK's dsp.c, over the microcode in software).
void DSPInit() {
    static bool done = false;
    if (done) return;
    done = true;
    RecursiveLock_Init(&sDspLock);
    withDsp([] { sDsp.reset(); return 0; });
    const BOOL enabled = OSDisableInterrupts();
    __OSSetInterruptHandler(__OS_INTERRUPT_DSP_DSP, __DSPHandler);
    __OSUnmaskInterrupts(OS_INTERRUPTMASK_DSP_DSP);
    OSRestoreInterrupts(enabled);
}
u32 DSPCheckMailToDSP() { return 0; }  // the DSP takes each mail as it is sent
u32 DSPCheckMailFromDSP() { return withDsp([] { return u32(sDsp.mailWaiting()); }); }
u32 DSPReadMailFromDSP() { return withDsp([] { return sDsp.readMail(); }); }
void DSPSendMailToDSP(u32 mail) { withDsp([mail] { sDsp.send(mail); return 0; }); }
void DSPAssertInt() {}

// AI (the SDK's ai.c, over NDSP).
void AIInit(u8*) {
    if (sEngine) return;
    sNdsp = R_SUCCEEDED(ndspInit());
    if (sNdsp) {
        LightEvent_Init(&sNdspFrame, RESET_ONESHOT);
        ndspSetCallback([](void*) { LightEvent_Signal(&sNdspFrame); }, nullptr);
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetRate(0, static_cast<float>(sSampleRate));
        sWaveMemory = static_cast<s16*>(linearAlloc(MaxBuffer * WaveBuffers));
        std::memset(sWaves, 0, sizeof sWaves);
        for (int i = 0; i < WaveBuffers; ++i) sWaves[i].data_pcm16 = sWaveMemory + i * MaxBuffer / 2, sWaves[i].status = NDSP_WBUF_FREE;
    } else {
        std::fputs("The 3DS's DSP (NDSP) would not start, so the game's audio runs silent (is dspfirm.cdc on the SD card?)\n", stderr);
    }
    const BOOL enabled = OSDisableInterrupts();
    __OSSetInterruptHandler(__OS_INTERRUPT_DSP_AI, aiInterrupt);
    __OSUnmaskInterrupts(OS_INTERRUPTMASK_DSP_AI);
    OSRestoreInterrupts(enabled);
    sEngine = threadCreate(engine, nullptr, 0x4000, 0x19, -2, true);
    if (!sEngine) {
        std::fputs("AIInit: the 3DS kernel made no thread for the audio DMA\n", stderr);
        std::abort();
    }
}
AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    const BOOL enabled = OSDisableInterrupts();
    const AIDCallback previous = sDmaCallback;
    sDmaCallback = callback;
    OSRestoreInterrupts(enabled);
    return previous;
}
void AIInitDMA(u32 start, u32 length) {
    if (length > MaxBuffer) {
        std::fprintf(stderr, "AIInitDMA: a %lu-byte block is larger than the port's %u\n", static_cast<unsigned long>(length), unsigned(MaxBuffer));
        std::abort();
    }
    sDmaStart = start, sDmaLength = length;
}
void AIStartDMA() { sPlaying = true; }
void AIStopDMA() { sPlaying = false; }
u32 AIGetDMAStartAddr() { return sDmaStart; }
u32 AIGetDMALength() { return sDmaLength; }
// AI_SAMPLERATE_32KHZ is 0, 48 kHz 1.
void AISetDSPSampleRate(u32 rate) {
    sSampleRate = rate ? 48000 : 32000;
    if (sNdsp) ndspChnSetRate(0, static_cast<float>(sSampleRate));
}
}
