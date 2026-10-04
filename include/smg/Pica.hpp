#pragma once
// What both of the port's renderers (the native scene's and the GX command processor's) hand the 3DS's PICA200.
#include <3ds.h>
#include <citro3d.h>
#include <smg/Combiner.hpp>
#include <smg/Model.hpp>
#include <cstdint>
#include <vector>

namespace smg {
// A texture converted and swizzled into the layout the PICA samples.
struct TiledTexture {
    unsigned width = 0, height = 0, wrapS = 0, wrapT = 0, minFilter = 1, magFilter = 1;
    GPU_TEXCOLOR format = GPU_RGBA8;
    std::vector<std::uint8_t> bytes;
};
// In the smallest native format that holds it without visible loss (tileFormat), or in `format` when given.
TiledTexture tile(const ModelTexture& source, int format = -1);
GPU_TEXCOLOR tileFormat(const ModelTexture& source);
bool upload(C3D_Tex& texture, const TiledTexture& tiled);

// The fragment side of a GX material: the combiner program as texture environments, and the pixel tests.
struct PicaPixel {
    C3D_TexEnv env[CombinerProgram::MaxStages];
    u32 bufferColor = 0;
    int bufferUpdate[2]{};
    bool alphaTest = false;
    GPU_TESTFUNC alphaFunc = GPU_ALWAYS;
    int alphaReference = 0;
    GPU_BLENDEQUATION equation = GPU_BLEND_ADD, alphaEquation = GPU_BLEND_ADD;
    GPU_BLENDFACTOR source = GPU_ONE, destination = GPU_ZERO, alphaSource = GPU_ONE, alphaDestination = GPU_ZERO;
    bool depthTest = true, depthWrite = true;
    GPU_TESTFUNC depthFunc = GPU_GEQUAL;
    void apply() const;
};
PicaPixel preparePixel(const ModelMaterial& material, const CombinerProgram& program);
} // namespace smg
