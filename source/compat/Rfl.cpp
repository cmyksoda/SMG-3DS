// The Wii's Mii library (RVLFaceLib) as it answers when its data can't be had: the 3DS holds no Wii Mii database,
// so initialising reports the library unavailable and no Mii exists. The game checks for exactly this and goes on
// without Mii faces. Drawing and models need a Mii, so they stay in source/compat/Sdk.cpp and stop by name.
#include <RVLFaceLib.h>
#include <cstdio>

extern "C" {
// The port's library holds nothing and never makes a Mii's model, so each asks for no more than one heap block.
u32 RFLGetWorkSize(BOOL) { return 32; }
u32 RFLGetModelBufferSize(RFLResolution, u32) { return 32; }

RFLErrcode RFLInitResAsync(void*, void*, u32, BOOL) {
    std::fputs("The Wii's Mii library: the 3DS has no Wii Mii database, so the game goes on without Miis\n", stderr);
    return RFLErrcode_NotAvailable;
}

RFLErrcode RFLGetAsyncStatus(void) { return RFLErrcode_NotAvailable; }
s32 RFLGetLastReason(void) { return 0; }
void RFLExit(void) {}
BOOL RFLIsAvailableOfficialData(u16) { return FALSE; }
BOOL RFLSearchOfficialData(const RFLCreateID*, u16*) { return FALSE; }
}
