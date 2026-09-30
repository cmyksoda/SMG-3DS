// The Wii's video interface on the 3DS's vertical blank. A thread just ahead of the game's own wakes each VBlank,
// counts the retrace and runs the game's retrace callbacks, as the VI interrupt did; the frame buffers the game
// hands over are kept as the registers kept them. What reaches the screens is the port's renderer's.
#include <3ds.h>
#include <revolution/vi.h>
#include <cstdio>
#include <cstdlib>

namespace {
LightLock sLock = 1;
CondVar sRetrace;
volatile u32 sRetraceCount = 0;
VIRetraceCallback sPreRetrace = nullptr, sPostRetrace = nullptr;
void* sNextFrameBuffer = nullptr;     // set by the game
void* sFlushedFrameBuffer = nullptr;  // committed by VIFlush
void* sCurrentFrameBuffer = nullptr;  // latched at the retrace
bool sBlack = true, sDimming = false;
Thread sThread;

void retraces(void*) {
    while (true) {
        gspWaitForEvent(GSPGPU_EVENT_VBlank0, true);
        LightLock_Lock(&sLock);
        const u32 count = ++sRetraceCount;
        // The flushed buffer takes over at the retrace, as the VI latched it.
        sCurrentFrameBuffer = sFlushedFrameBuffer;
        const VIRetraceCallback pre = sPreRetrace, post = sPostRetrace;
        LightLock_Unlock(&sLock);
        if (pre) pre(count);
        if (post) post(count);
        LightLock_Lock(&sLock);
        CondVar_Broadcast(&sRetrace);
        LightLock_Unlock(&sLock);
    }
}
} // namespace

extern "C" {
void VIInit() {
    if (sThread) return;
    CondVar_Init(&sRetrace);
    sThread = threadCreate(retraces, nullptr, 0x4000, 0x2E, -2, true);
    if (!sThread) {
        std::fprintf(stderr, "VIInit: the 3DS kernel made no thread for the retrace\n");
        std::abort();
    }
}

void VIWaitForRetrace() {
    LightLock_Lock(&sLock);
    const u32 start = sRetraceCount;
    while (sRetraceCount == start) CondVar_Wait(&sRetrace, &sLock);
    LightLock_Unlock(&sLock);
}

u32 VIGetRetraceCount() { return sRetraceCount; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    LightLock_Lock(&sLock);
    const VIRetraceCallback previous = sPreRetrace;
    sPreRetrace = callback;
    LightLock_Unlock(&sLock);
    return previous;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    LightLock_Lock(&sLock);
    const VIRetraceCallback previous = sPostRetrace;
    sPostRetrace = callback;
    LightLock_Unlock(&sLock);
    return previous;
}

// The mode sets the Wii's TV timing, which the 3DS's own screens replace.
void VIConfigure(const GXRenderModeObj*) {}
void VISetNextFrameBuffer(void* buffer) { sNextFrameBuffer = buffer; }
void* VIGetNextFrameBuffer() { return sNextFrameBuffer; }
void* VIGetCurrentFrameBuffer() { return sCurrentFrameBuffer; }
void VIFlush() {
    LightLock_Lock(&sLock);
    sFlushedFrameBuffer = sNextFrameBuffer;
    LightLock_Unlock(&sLock);
}
void VISetBlack(BOOL black) { sBlack = black; }
// An NTSC console on an analog cable, which is what GXNtsc480Int describes.
u32 VIGetTvFormat() { return VI_NTSC; }
u32 VIGetDTVStatus() { return 0; }
void VISetTrapFilter(VIBool) {}

// Screen dimming protects a TV from burn-in after idle minutes; the 3DS dims its own backlight instead, so no
// idle time is ever counted here.
BOOL VIEnableDimming(BOOL enable) {
    const BOOL previous = sDimming;
    sDimming = enable;
    return previous;
}
u32 VIGetDimmingCount() { return 0; }
}
