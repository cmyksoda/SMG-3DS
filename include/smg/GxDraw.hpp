#pragma once
#include <smg/Combiner.hpp>
#include <smg/GxCommands.hpp>
#include <smg/Model.hpp>
#include <cstddef>
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
// as citro3d's projections), turned for the 3DS's screen when the frame says; its channel colour, and each combiner
// unit's coordinates.
struct GxDrawVertex {
    float position[4];
    std::uint8_t color[4];
    float texcoord[CombinerProgram::MaxUnits][2];
};

// The region of the EFB the port shows, in EFB pixels. quarterTurn turns positions as the 3DS's top screen is turned
// (x takes y, y takes minus x, as citro3d's tilted projections).
struct GxFrame {
    float width = 640, height = 480;
    bool quarterTurn = false;
    bool operator==(const GxFrame& o) const { return width == o.width && height == o.height && quarterTurn == o.quarterTurn; }
};

// The state a run of primitives is drawn with, as the Wii GPU's BP registers hold it: the TEV program and pixel tests as a
// ModelMaterial (for compileTev), texture maps, scissor. `unsupported` names the first GX feature this can't reproduce
// yet; nothing should be drawn then. It changes only when the processor's bpChanges does.
struct GxDrawState {
    ModelMaterial material;
    CombinerProgram program;
    GxTextureMap textures[8];
    bool colorUpdate = true, alphaUpdate = true;
    // BP 0xF4/0xF5: depth from the last TEV stage's texture (op 1 add, 2 replace), its texel size 0 u8, 1 u16, 2 u24.
    std::uint8_t depthTextureOp = 0, depthTextureType = 0;
    std::uint32_t depthTextureOffset = 0;
    // BP 0x42 (GXSetDstAlpha): the EFB keeps this alpha instead of the TEV's, which still blends and is tested.
    bool dstAlphaEnable = false;
    std::uint8_t dstAlpha = 0;
    // Set by whoever reads the depth texture: every vertex at this depth (0 near, 1 far).
    bool fixedDepth = false;
    float depth01 = 0;
    int scissor[4] = {};  // left, top, right, bottom (exclusive), EFB pixels
    // A stage's indirect offset left out, its regular coordinates kept: the PICA has no dependent texture reads.
    bool indirectDropped = false;
    // The program reads TEV register C0 as the vertex colour, which then holds C0: a primitive whose C0 alone differs
    // (a particle's fading colour) joins the same draw instead of starting another.
    bool color0InVertex = false;
    const char* unsupported = nullptr;
};
void readGxDrawState(const GxCommandProcessor& gpu, GxDrawState& out);
// C0 as four bytes, false when a component is outside 0-255 (then it can't be a vertex colour).
bool gxColor0Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]);
// TEV programs compiled so far (readGxDrawState keeps each by what compileTev reads): how often its cache missed.
std::uint32_t gxTevCompiles();

// One primitive as triangles, for tests and tools: the state, and every surviving triangle's three vertices.
struct GxDraw : GxDrawState {
    std::vector<GxDrawVertex> triangles;
};

// GX's lighting for colour channel 0 as XF's control words, material and ambient registers and lights hold it, decoded
// once so each vertex only does the arithmetic (Dolphin's software renderer is the reference for it). apply() takes a
// vertex's view-space position, unit normal and colour 0.
struct GxLighting {
    struct Part {
        bool lit = false, materialFromVertex = false, ambientFromVertex = false, attenuated = false, spot = false;
        std::uint8_t diffuse = 0, mask = 0;
    };
    struct Light {
        float color[4], cosine[3], distance[3], position[3], direction[3];
    };
    Part parts[2];  // colour, alpha
    bool shared = false;  // both lit by the same lights the same way: each light's attenuation worked out once
    std::uint8_t material[4] = {}, ambient[4] = {};
    Light lights[8] = {};
    void read(const GxCommandProcessor& gpu);
    void apply(const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]) const;
};

// Turns primitives into vertices for the PICA, transformed through XF on the CPU, and the triangles that survive GX's
// culling as indices. What it learns from the processor (vertex layouts, each matrix slot's whole transform) it keeps
// until the matching change count moves.
class GxVertexPipeline {
public:
    // Readies a primitive's kind and format under `state`; false (and `unsupported` set) when it can't be drawn.
    bool prepare(const GxCommandProcessor& gpu, const GxDrawState& state, const GxFrame& frame, std::uint8_t kind, std::uint8_t format);
    // The prepared primitive's `count` vertices into `vertices`, and its triangles appended to `indices` as vertex numbers
    // from `first`: how many indices that wrote, at most maxIndices(count).
    std::size_t run(const std::uint8_t* data, std::uint16_t count, GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices);
    static constexpr std::size_t maxIndices(std::size_t count) { return 3 * count; }
    // The state passed to prepare was read again: the next prepare reads everything.
    void stateChanged() { prepared_ = false; }
    const char* unsupported = nullptr;

private:
    struct TexGen {
        std::uint8_t source = 0, coord = 0, rows = 2;
        bool threeInputs = false, perVertexMatrix = false, dual = false, normalize = false;
        std::uint32_t matrix = 0, post = 0;
        // With one matrix for the whole primitive and no normalizing, the texgen is one affine map: the matrix and the
        // dual transform folded together (the input's fourth component is always one).
        bool folded = false, planar = false;  // planar: two inputs, two outputs, nothing to divide by
        float fold[3][4] = {};
        // What the fold was made from (setup, then the matrices' words), and the XF change count it was checked at.
        std::uint32_t source_[32] = {}, sourceWords = 0, foldXf = ~0u;
    };
    const float* clipMatrix(std::uint32_t index) { return index < 64 && clipReady_ >> index & 1 ? clip_[index] : makeClipMatrix(index); }
    const float* makeClipMatrix(std::uint32_t index);
    bool foldStillHolds(TexGen& t, std::uint32_t ctrl, std::uint32_t post) const;
    void texgen(const TexGen& t, const GxVertex& v, float st[2]) const;
    float matrix(std::uint32_t row, int column) const;

    // What the last prepare saw: a primitive with nothing changed since skips it (most of a model's do).
    bool prepared_ = false;
    std::uint8_t preparedKind_ = 0, preparedFormat_ = 0;
    std::uint32_t preparedBp_ = 0, preparedXf_ = 0, preparedCp_ = 0;
    GxFrame preparedFrame_;
    const GxCommandProcessor* gpu_ = nullptr;
    const GxDrawState* state_ = nullptr;
    const GxVertexLayout* layout_ = nullptr;
    std::uint8_t kind_ = 0, reject_ = 0, units_ = 0;
    bool perVertexMatrix_ = false, channel_ = false, lit_ = false, fixedDepth_ = false, color0InVertex_ = false;
    std::uint8_t color0_[4] = {};
    std::uint32_t defaultMatrix_ = 0;
    float depth01_ = 0;
    TexGen texgens_[CombinerProgram::MaxUnits];
    // From view space to what the PICA takes, and each matrix slot's position matrix through it, while XF and the frame stay.
    std::uint32_t xfSeen_ = ~0u, viewSeen_[13] = {};  // XF 0x101A-0x1026: the viewport and projection
    GxFrame frameSeen_{0, 0, false};
    double toClip_[4][4] = {};
    float clip_[64][16] = {}, clipScratch_[16] = {};
    std::uint64_t clipReady_ = 0;
    GxLighting lighting_;
    std::uint32_t lightingXf_ = ~0u;
};

// GX's lighting for colour channel 0 at one vertex, straight from the processor's state (GxLighting, read and applied).
void gxLitChannel(const GxCommandProcessor& gpu, const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]);

// The state and one primitive's triangles at once (a fresh pipeline each call).
void buildGxDraw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices,
                 const GxFrame& frame, GxDraw& out);
} // namespace smg
