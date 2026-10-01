#pragma once
#include <smg/Combiner.hpp>
#include <smg/GxCommands.hpp>
#include <smg/Model.hpp>
#include <cstdint>
#include <vector>

namespace smg {
// One texture map as BP's TX registers set it: the image where the GPU would fetch it from the Wii's memory.
struct GxTextureMap {
    std::uint32_t address = 0;  // physical
    std::uint16_t width = 0, height = 0;
    std::uint8_t format = 0, wrapS = 0, wrapT = 0, minFilter = 0, magFilter = 0;
};

// A vertex ready for the PICA: clip space with the EFB mapped onto [-1, 1] (y up) and depth onto [-1, 0] (near to far,
// as citro3d's projections), before the 3DS screen's rotation; its channel colour, and each combiner unit's coordinates.
struct GxDrawVertex {
    float position[4];
    std::uint8_t color[4];
    float texcoord[CombinerProgram::MaxUnits][2];
};

// The region of the EFB the port shows, in EFB pixels.
struct GxFrame {
    float width = 640, height = 480;
};

// A primitive as the Wii's GPU would draw it with the state its registers hold: the TEV program and pixel tests as a
// ModelMaterial (for compileTev), texture maps, and triangles transformed through XF. `unsupported` names the first
// GX feature this can't reproduce yet; nothing should be drawn then.
struct GxDraw {
    ModelMaterial material;
    CombinerProgram program;
    GxTextureMap textures[8];
    bool colorUpdate = true, alphaUpdate = true;
    // BP 0xF4/0xF5: depth from the last TEV stage's texture (op 1 add, 2 replace), its texel size 0 u8, 1 u16, 2 u24.
    std::uint8_t depthTextureOp = 0, depthTextureType = 0;
    std::uint32_t depthTextureOffset = 0;
    int scissor[4] = {};  // left, top, right, bottom (exclusive), EFB pixels
    // A stage's indirect offset left out, its regular coordinates kept: the PICA has no dependent texture reads.
    bool indirectDropped = false;
    std::vector<GxDrawVertex> triangles;
    const char* unsupported = nullptr;
};

void buildGxDraw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices,
                 const GxFrame& frame, GxDraw& out);
} // namespace smg
