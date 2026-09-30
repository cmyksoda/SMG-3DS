// The Wii GPU API that J3D's drawing side calls. The port draws with citro3d, and nothing reaches these;
// each stops the program by name if it ever does. See docs/PORTING.md, "Road to Mario".
#include <revolution/gd/GDGeometry.h>
#include <revolution/gx/GXBump.h>
#include <revolution/gx/GXCull.h>
#include <revolution/gx/GXDispList.h>
#include <revolution/gx/GXGeometry.h>
#include <revolution/gx/GXLighting.h>
#include <revolution/gx/GXPixel.h>
#include <revolution/gx/GXTev.h>
#include <revolution/gx/GXTexture.h>
#include <revolution/gx/GXTransform.h>
#include <revolution/gx/GXVert.h>
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void unported(const char* what) {
    std::fprintf(stderr, "The Wii GPU is not the port's: %s\n", what);
    std::abort();
}
} // namespace

void GXWGFifoWrite() { unported("a write to GXWGFifo"); }
void GDSetArray(GXAttr, const void*, u8) { unported("GDSetArray"); }
void GDSetArrayRaw(GXAttr, u32, u8) { unported("GDSetArrayRaw"); }
void GDSetVtxDescv(const GXVtxDescList*) { unported("GDSetVtxDescv"); }
void GXCallDisplayList(const void *, u32) { unported("GXCallDisplayList"); }
void GXInitTexCacheRegion(GXTexRegion *, GXBool, u32, GXTexCacheSize, u32, GXTexCacheSize) { unported("GXInitTexCacheRegion"); }
void GXInitTexObj(GXTexObj *, void *, u16, u16, GXTexFmt, GXTexWrapMode, GXTexWrapMode, GXBool) { unported("GXInitTexObj"); }
void GXInvalidateVtxCache() { unported("GXInvalidateVtxCache"); }
void GXLoadTexMtxImm(const f32[][4], u32, GXTexMtxType) { unported("GXLoadTexMtxImm"); }
void GXLoadTexObj(const GXTexObj *, GXTexMapID) { unported("GXLoadTexObj"); }
void GXSetAlphaCompare(GXCompare, u8, GXAlphaOp, GXCompare, u8) { unported("GXSetAlphaCompare"); }
void GXSetAlphaUpdate(GXBool) { unported("GXSetAlphaUpdate"); }
void GXSetArray(GXAttr, const void *, u8) { unported("GXSetArray"); }
void GXSetBlendMode(GXBlendMode, GXBlendFactor, GXBlendFactor, GXLogicOp) { unported("GXSetBlendMode"); }
void GXSetChanAmbColor(GXChannelID, GXColor) { unported("GXSetChanAmbColor"); }
void GXSetChanCtrl(GXChannelID, GXBool, GXColorSrc, GXColorSrc, u32, GXDiffuseFn, GXAttnFn) { unported("GXSetChanCtrl"); }
void GXSetChanMatColor(GXChannelID, GXColor) { unported("GXSetChanMatColor"); }
void GXSetClipMode(GXClipMode) { unported("GXSetClipMode"); }
void GXSetColorUpdate(GXBool) { unported("GXSetColorUpdate"); }
void GXSetCoPlanar(GXBool) { unported("GXSetCoPlanar"); }
void GXSetCullMode(GXCullMode) { unported("GXSetCullMode"); }
void GXSetCurrentMtx(u32) { unported("GXSetCurrentMtx"); }
void GXSetDither(GXBool) { unported("GXSetDither"); }
void GXSetFog(GXFogType, f32, f32, f32, f32, GXColor) { unported("GXSetFog"); }
void GXSetFogRangeAdj(GXBool, u16, const GXFogAdjTable *) { unported("GXSetFogRangeAdj"); }
void GXSetIndTexCoordScale(GXIndTexStageID, GXIndTexScale, GXIndTexScale) { unported("GXSetIndTexCoordScale"); }
void GXSetIndTexMtx(GXIndTexMtxID, const f32[2][3], s8) { unported("GXSetIndTexMtx"); }
void GXSetIndTexOrder(GXIndTexStageID, GXTexCoordID, GXTexMapID) { unported("GXSetIndTexOrder"); }
void GXSetNumChans(u8) { unported("GXSetNumChans"); }
void GXSetNumIndStages(u8) { unported("GXSetNumIndStages"); }
void GXSetNumTevStages(u8) { unported("GXSetNumTevStages"); }
void GXSetNumTexGens(u8) { unported("GXSetNumTexGens"); }
void GXSetTevAlphaIn(GXTevStageID, GXTevAlphaArg, GXTevAlphaArg, GXTevAlphaArg, GXTevAlphaArg) { unported("GXSetTevAlphaIn"); }
void GXSetTevAlphaOp(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool, GXTevRegID) { unported("GXSetTevAlphaOp"); }
void GXSetTevColor(GXTevRegID, GXColor) { unported("GXSetTevColor"); }
void GXSetTevColorIn(GXTevStageID, GXTevColorArg, GXTevColorArg, GXTevColorArg, GXTevColorArg) { unported("GXSetTevColorIn"); }
void GXSetTevColorOp(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool, GXTevRegID) { unported("GXSetTevColorOp"); }
void GXSetTevColorS10(GXTevRegID, GXColorS10) { unported("GXSetTevColorS10"); }
void GXSetTevDirect(GXTevStageID) { unported("GXSetTevDirect"); }
void GXSetTevIndirect(GXTevStageID, GXIndTexStageID, GXIndTexFormat, GXIndTexBiasSel, GXIndTexMtxID, GXIndTexWrap, GXIndTexWrap, GXBool, GXBool, GXIndTexAlphaSel) { unported("GXSetTevIndirect"); }
void GXSetTevKAlphaSel(GXTevStageID, GXTevKAlphaSel) { unported("GXSetTevKAlphaSel"); }
void GXSetTevKColor(GXTevKColorID, GXColor) { unported("GXSetTevKColor"); }
void GXSetTevKColorSel(GXTevStageID, GXTevKColorSel) { unported("GXSetTevKColorSel"); }
void GXSetTevOrder(GXTevStageID, GXTexCoordID, GXTexMapID, GXChannelID) { unported("GXSetTevOrder"); }
void GXSetTevSwapMode(GXTevStageID, GXTevSwapSel, GXTevSwapSel) { unported("GXSetTevSwapMode"); }
void GXSetTevSwapModeTable(GXTevSwapSel, GXTevColorChan, GXTevColorChan, GXTevColorChan, GXTevColorChan) { unported("GXSetTevSwapModeTable"); }
void GXSetTexCoordGen2(GXTexCoordID, GXTexGenType, GXTexGenSrc, u32, GXBool, u32) { unported("GXSetTexCoordGen2"); }
void GXSetVtxAttrFmt(GXVtxFmt, GXAttr, GXCompCnt, GXCompType, u8) { unported("GXSetVtxAttrFmt"); }
void GXSetZCompLoc(GXBool) { unported("GXSetZCompLoc"); }
void GXSetZMode(GXBool, GXCompare, GXBool) { unported("GXSetZMode"); }

// J3D's own drawing entry points that Petari has no body for yet.
#include <JSystem/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
void J3DPacket::draw() { unported("J3DPacket::draw"); }
void J3DShapeMtx::loadMtxIndx_PNGP(int, u16) const { unported("J3DShapeMtx::loadMtxIndx_PNGP"); }
