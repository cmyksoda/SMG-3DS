// The Wii's audio hardware as the SDK's DSP and AI libraries saw it. The DSP runs JAudio2's microcode in software
// (smg::WiiDsp) behind its mailboxes, on a thread of its own on the system core when the 3DS lends it one, as the Wii's
// DSP worked beside its CPU; the AI's DMA plays each buffer the game hands it through the 3DS's own DSP (NDSP) and
// interrupts as each one starts, so JAudio2's audio thread paces itself as on the Wii.
#include <3ds.h>
#include <smg/FreeStore.hpp>
#include <smg/WiiDsp.hpp>
#include <revolution/ai.h>
#include <revolution/dsp.h>
#include <revolution/os.h>
#include <revolution/os/OSInterrupt.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void smgRaiseInterrupt(__OSInterrupt interrupt);
extern "C" void __DSPHandler(__OSInterrupt interrupt, OSContext* context);  // JAudio2's (osdsp_task.cpp)
// Ticks spent running the DSP's microcode, for the renderer's perf line: time the game's thread shares its core with.
std::atomic<u32> smgDspTicks{0};

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
    const u64 start = svcGetSystemTick();
    auto result = call();
    smgDspTicks.fetch_add(static_cast<u32>(svcGetSystemTick() - start), std::memory_order_relaxed);
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

// The CPU's mails to the DSP, run in order by the DSP's thread: the sender goes on (waiting only if the queue is full)
// and the microcode's work, mixing included, runs on the other core.
class Mailbox {
public:
    void push(u32 mail) {
        LightLock_Lock(&lock_);
        while (count_ == Capacity) CondVar_Wait(&taken_, &lock_);
        mails_[(first_ + count_++) % Capacity] = mail;
        CondVar_Signal(&arrived_);
        LightLock_Unlock(&lock_);
    }
    u32 take() {
        LightLock_Lock(&lock_);
        while (!count_) CondVar_Wait(&arrived_, &lock_);
        const u32 mail = mails_[first_];
        first_ = (first_ + 1) % Capacity, --count_;
        CondVar_Broadcast(&taken_);
        LightLock_Unlock(&lock_);
        return mail;
    }
private:
    static constexpr unsigned Capacity = 64;
    LightLock lock_ = 1;
    CondVar arrived_ = 0, taken_ = 0;
    u32 mails_[Capacity] = {};
    unsigned first_ = 0, count_ = 0;
};
Mailbox sToDsp;
Thread sDspThread = nullptr;
void* sDspTls = nullptr;

void dspLoop(void*) {
    sDspTls = getThreadLocalStorage();
    for (;;) {
        const u32 mail = sToDsp.take();
        withDsp([mail] { sDsp.send(mail); return 0; });
    }
}

// The system core, at the app's largest share of it (only the New 3DS's third core is free, and the GX stream has it).
void startDspThread() {
    if (R_FAILED(APT_SetAppCpuTimeLimit(80))) return void(std::fputs("The Wii's DSP: run on the game's core (no share of the system core)\n", stderr));
    sDspThread = threadCreate(dspLoop, nullptr, 0x8000, 0x18, 1, true);
    std::fputs(sDspThread ? "The Wii's DSP: run on core 1\n" : "The Wii's DSP: run on the game's core (no thread on core 1)\n", stderr);
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

// The first block with sound in it, said once in the log: the proof that the game's mixing reaches the output.
void noteFirstSound(const s16* samples, u32 count) {
    static bool heard = false;
    static u32 blocks = 0;
    if (heard) return;
    ++blocks;
    int peak = 0;
    for (u32 i = 0; i < count; ++i) peak = std::max(peak, std::abs(int(samples[i])));
    if (peak == 0) return;
    heard = true;
    std::fprintf(stderr, "The game's audio: first sound in DMA block %lu (peak %d of 32767)\n", static_cast<unsigned long>(blocks), peak);
}

// Copies the buffer the game handed over into the next wave, as the DMA took it, and tells the game it began.
void startBlock(ndspWaveBuf* wave) {
    const u32 length = std::min<u32>(u32(sDmaLength), MaxBuffer);
    if (sDmaStart && length) noteFirstSound(reinterpret_cast<const s16*>(static_cast<uintptr_t>(sDmaStart)), length / 2);
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
    startDspThread();
    const BOOL enabled = OSDisableInterrupts();
    __OSSetInterruptHandler(__OS_INTERRUPT_DSP_DSP, __DSPHandler);
    __OSUnmaskInterrupts(OS_INTERRUPTMASK_DSP_DSP);
    OSRestoreInterrupts(enabled);
}
// The queue takes each mail as it is sent: the Wii's one-mail box made its sender wait (spinning, interrupts off),
// which here would hold the game's core until the other one got to it. Mails stay in order.
u32 DSPCheckMailToDSP() { return 0; }
u32 DSPCheckMailFromDSP() { return withDsp([] { return u32(sDsp.mailWaiting()); }); }
u32 DSPReadMailFromDSP() { return withDsp([] { return sDsp.readMail(); }); }
// A mail from an interrupt handler running on the DSP's own thread (which raised the interrupt) runs at once, as the
// thread can't take its own mail while it runs the handler; the handler holds the CPU, so the queue is in order.
void DSPSendMailToDSP(u32 mail) {
    if (sDspThread && getThreadLocalStorage() != sDspTls) sToDsp.push(mail);
    else withDsp([mail] { sDsp.send(mail); return 0; });
}
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
