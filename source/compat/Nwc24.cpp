// WiiConnect24 carries the game's letters to the Wii Message Board, which the 3DS lacks: every call answers as a Wii
// with it switched off, and the game goes on without sending. C linkage matches by name, so no SDK header is needed.
#include <revolution/nwc24/NWC24Types.h>

extern "C" {
// The virtual drive that holds WiiConnect24's mail; with the service off there is nothing to mount.
void VFInitEx(void*, u32) {}

NWC24Err NWC24OpenLib() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24CloseLib() { return NWC24_ERR_DISABLED; }
// The detail code the system menu would show; none, for a service that is off rather than failing.
s32 NWC24GetErrorCode() { return 0; }
NWC24Err NWC24GetMyUserId() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24InitMsgObj() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgToId() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgText() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgAltName() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgAttached() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgTag() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgLedPattern() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgMBDelay() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24SetMsgMBNoReply() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24GetMsgSize() { return NWC24_ERR_DISABLED; }
NWC24Err NWC24CommitMsg() { return NWC24_ERR_DISABLED; }
}
