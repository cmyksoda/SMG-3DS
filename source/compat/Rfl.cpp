// The Wii's Mii library (RVLFaceLib) as it answers when its data can't be had: the 3DS holds no Wii Mii database,
// so initialising reports the library unavailable and no Mii exists. The game checks for exactly this and goes on
// without Mii faces. The GX set-up the game calls before its (empty) Mii pass is RVLFaceLib's own; drawing and models
// need a Mii, so they stay in source/compat/Sdk.cpp and stop by name.
#include <RVLFaceLib.h>
#include <revolution/gx.h>
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
