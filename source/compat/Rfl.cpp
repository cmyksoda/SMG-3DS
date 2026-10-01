// The Wii's Mii library (RVLFaceLib) answering as a Wii does where no Mii was ever made: the 3DS holds no Wii Mii
// database, so the library starts and its database is empty. (Jaxi's call pending; reporting the database unreadable
// instead makes the file select show "Unable to read the Mii Channel data" at every boot.) Every request for a Mii
// gets the library's own answer for an empty slot. The GX set-up for the Mii pass is RVLFaceLib's; drawing needs a
// Mii, so it stays in source/compat/Sdk.cpp and stops by name.
#include <RVLFaceLib.h>
#include <RFLi_Model.h>
#include <revolution/mtx.h>
#include <revolution/gx.h>
#include <cstdio>
#include <cstdlib>

extern "C" u32 PSMTXInvXpose(const Mtx, Mtx);  // source/compat/Math.cpp; the SDK's header leaves it out

namespace {
// RFLiGetCharInfo's answer for an official slot, all of them empty here. A default Mii (RVLFaceLib's built-in ones)
// can't be made without the library itself; the file select only keeps one as a hidden placeholder face, shown for a
// file whose icon is a Mii, which no file can have here.
RFLErrcode emptySlot(RFLDataSource source, u16 index, const char* who) {
    if (source == RFLDataSource_Default) return RFLErrcode_NotAvailable;
    if (source != RFLDataSource_Official) {
        std::fprintf(stderr, "The Wii's Mii library: %s of a Mii from source %d is not the port's yet\n", who, int(source));
        std::abort();
    }
    return index >= RFL_DB_CHAR_MAX ? RFLErrcode_WrongParam : RFLErrcode_NotAvailable;
}
} // namespace

extern "C" {
// The port's library holds nothing and never makes a Mii's model, so each asks for no more than one heap block.
u32 RFLGetWorkSize(BOOL) { return 32; }
u32 RFLGetModelBufferSize(RFLResolution, u32) { return 32; }

RFLErrcode RFLInitResAsync(void*, void*, u32, BOOL) {
    std::fputs("The Wii's Mii library: the 3DS has no Wii Mii database, so the game sees one with no Miis\n", stderr);
    return RFLErrcode_Success;
}

RFLErrcode RFLGetAsyncStatus(void) { return RFLErrcode_Success; }
s32 RFLGetLastReason(void) { return 0; }
void RFLExit(void) {}
BOOL RFLIsAvailableOfficialData(u16) { return FALSE; }
BOOL RFLSearchOfficialData(const RFLCreateID*, u16*) { return FALSE; }

RFLErrcode RFLGetAdditionalInfo(RFLAdditionalInfo*, RFLDataSource source, RFLMiddleDB*, u16 index) {
    return emptySlot(source, index, "RFLGetAdditionalInfo");
}
RFLErrcode RFLMakeIcon(void*, RFLDataSource source, RFLMiddleDB*, u16 index, RFLExpression, const RFLIconSetting*) {
    return emptySlot(source, index, "RFLMakeIcon");
}
RFLErrcode RFLInitCharModel(RFLCharModel*, RFLDataSource source, RFLMiddleDB*, u16 index, void*, RFLResolution, u32) {
    return emptySlot(source, index, "RFLInitCharModel");
}

// A model's matrices and expression are its own state, as RFL_Model.c keeps them (the file select's hidden faces set
// theirs every frame).
void RFLSetMtx(RFLCharModel* model, const Mtx mvMtx) {
    RFLiCharModel* imodel = reinterpret_cast<RFLiCharModel*>(model);
    PSMTXCopy(mvMtx, imodel->posMtx);
    PSMTXInvXpose(mvMtx, imodel->nrmMtx);
}
void RFLSetExpression(RFLCharModel* model, RFLExpression expr) { reinterpret_cast<RFLiCharModel*>(model)->expression = expr; }

// As RFL_Model.c sets them: indexed s16 positions, normals and texture coordinates (index type 2 is GX_INDEX8).
void RFLLoadVertexSetting(const RFLDrawCoreSetting* setting) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
    GXSetVtxDesc(GX_VA_NRM, GX_INDEX8);
    GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 8);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_S16, 14);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_S16, 13);
    GXSetNumTexGens(setting->txcGenNum);
}

void RFLLoadMaterialSetting(const RFLDrawCoreSetting* setting) {
    GXSetTevSwapModeTable(setting->tevSwapTable, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapModeTable(GXTevSwapSel(setting->tevSwapTable + 1), GX_CH_RED, GX_CH_ALPHA, GX_CH_BLUE, GX_CH_GREEN);
    GXSetNumTevStages(setting->tevStageNum);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, setting->tevOutRegID);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, setting->tevOutRegID);
    GXSetTevKColorSel(GX_TEVSTAGE0, GXTevKColorSel(setting->tevKColorID + GX_TEV_KCSEL_K0));
    GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_8_8);
}
}
