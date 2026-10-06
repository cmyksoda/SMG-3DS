#pragma once
#include <smg/Combiner.hpp>
#include <smg/GxCommands.hpp>
#include <smg/Model.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace smg {
// One texture map as BP's TX registers set it: the image where the GPU would fetch it from the Wii's memory.
struct GxTextureMap {
    std::uint32_t address = 0;  // physical
    std::uint16_t width = 0, height = 0;
    std::uint8_t format = 0, wrapS = 0, wrapT = 0, minFilter = 0, magFilter = 0;
    std::uint8_t maxLod = 0, lodBias = 0;  // TX_SETMODE1's (sixteenths of a level), TX_SETMODE0's (signed, 32nds)
};

// A vertex ready for the PICA: clip space with the EFB mapped onto [-1, 1] (y up) and depth onto [-1, 0] (near to far,
// as citro3d's projections), turned for the 3DS's screen when the frame says; its channel color, and each combiner
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
    // Why the draws under this state are left out (null if they aren't): a stage's coordinates replaced by an indirect
    // lookup, which would read one texel everywhere instead.
    const char* leftOut = nullptr;
    // The program reads TEV register C0 as the vertex color, which then holds C0: a primitive whose C0 alone differs
    // (a particle's fading color) joins the same draw instead of starting another.
    bool color0InVertex = false;
    // With C0 riding, the program reads C1 from a palette texel on unit paletteUnit (the CPU path's draws only): a
    // particle's other color no longer starts a draw either.
    bool color1InTexture = false;
    std::uint8_t paletteUnit = 0;
    // Passes drawn after the program's over the same primitives (splitTev, splitAlphaTest): a material one PICA pass can't
    // hold, such as the game's water with its bright lines and more samples than units, or an alpha test of two compares.
    struct Pass {
        CombinerProgram program;
        TevPass::Blend blend = TevPass::AddTimesAlpha;
        std::uint8_t reference = 0;
        bool writesAlpha = false;
        bool sameUnits = false;  // its units sample what the program's do, unit by unit: the same vertices serve it
        std::uint8_t compare = 7;  // Retest: the GX compare of its alpha byte against reference
    };
    std::vector<Pass> passes;
    const char* unsupported = nullptr;
};
// palette: C1 may come from a palette unit (color1InTexture), for draws on the CPU path.
void readGxDrawState(const GxCommandProcessor& gpu, GxDrawState& out, bool palette = false);
// The registers readGxDrawState reads for the stages and texture maps in use, in a fixed order, as a cache key: two
// processors with the same key read the same state (a map no stage uses may differ in it). A C0 that can ride in the
// vertex color, or with the palette a C1 in its texel, is a marker instead (bits 1 and 2 of `markers`): the same state
// holds for every value unless readGxDrawState kept the value in its program, which the caller checks.
// At most MaxDrawStateKey words; the texture maps' words come last, three a map (mode, image size, image address), from
// `mapsAt` when given.
constexpr std::size_t MaxDrawStateKey = 128;
std::size_t gxDrawStateKey(const GxCommandProcessor& gpu, std::uint32_t* out, bool palette = false, unsigned* markers = nullptr,
                           std::size_t* mapsAt = nullptr);
// C0 as four bytes, false when a component is outside 0-255 (then it can't be a vertex color).
bool gxColor0Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]);
bool gxColor1Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]);
// TEV programs compiled so far (readGxDrawState keeps each by what compileTev reads): how often its cache missed.
std::uint32_t gxTevCompiles();

// One primitive as triangles, for tests and tools: the state, and every surviving triangle's three vertices.
struct GxDraw : GxDrawState {
    std::vector<GxDrawVertex> triangles;
};

// GX's lighting for color channel 0 as XF's control words, material and ambient registers and lights hold it, decoded
// once so each vertex only does the arithmetic (Dolphin's software renderer is the reference for it). apply() takes a
// vertex's view-space position, unit normal and color 0.
struct GxLighting {
    struct Part {
        bool lit = false, materialFromVertex = false, ambientFromVertex = false, attenuated = false, spot = false;
        std::uint8_t diffuse = 0, mask = 0;
    };
    struct Light {
        float color[4], cosine[3], distance[3], position[3], direction[3];
    };
    Part parts[2];  // color, alpha
    bool shared = false;  // both lit by the same lights the same way: each light's attenuation worked out once
    std::uint8_t material[4] = {}, ambient[4] = {};
    Light lights[8] = {};
    void read(const GxCommandProcessor& gpu);
    void apply(const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]) const;
};

// The GPU path: a display list of primitives decoded once, in model space, for the PICA's vertex shader to transform
// every time it is called. What the shader reads of each vertex is its position, color, each texgen's input vector
// (the CPU path's `in`, its fourth component one), its normal (for lighting) and its matrix slot's first uniform row:
// slot x 4 unlit (each slot's four clip rows); lit, 4 + its group's compact slot x 6 (modelview and normal rows, after
// the four rows from view to clip the slots share). GxMeshVertex holds all of that; a mesh packs only what its draws use.
struct GxMeshVertex {
    float position[3];
    std::uint8_t color[4];
    float source[CombinerProgram::MaxUnits][3];
    float normal[3];
    float slot;
};
// How a mesh packs its vertices: the position (three floats), the color (four bytes) when it comes from the vertex, each
// texgen input that is data of its own, the normal when lit or a texgen reads it, the slot's float for per-vertex matrices.
// An input that is the position or the normal is those bytes read again; a two-input texgen holds two floats, its third folded.
struct GxMeshLayout {
    static constexpr std::uint8_t Absent = 0xFF;
    enum Input : std::uint8_t { Unused, Stored, OfPosition, OfNormal };
    struct Unit {
        Input input = Unused;
        std::uint8_t floats = 3, at = Absent, source = 0;  // at: where Stored; source: the texgen's XF source row
    };
    // One texgen unit as the layout needs it: its XF source row (0 position, 1 normal, 5 + n texcoord n), whether its
    // input form has three inputs, and whether its matrix comes from the vertex.
    struct Texgen {
        std::uint8_t source;
        bool threeInputs, perVertexMatrix;
    };
    std::uint8_t stride = 12, color = Absent, normal = Absent, slot = Absent;
    bool lit = false;
    Unit unit[CombinerProgram::MaxUnits];
    static GxMeshLayout make(bool colorFromVertex, bool lit, bool slot, unsigned units, const Texgen* texgens);
    bool operator==(const GxMeshLayout& o) const { return !std::memcmp(this, &o, sizeof *this); }
    bool operator!=(const GxMeshLayout& o) const { return !(*this == o); }
    // The same loaders: layouts that differ only in which texcoord a stored input comes from share their buffers.
    bool sameBuffers(const GxMeshLayout& o) const {
        GxMeshLayout a = *this, b = o;
        for (Unit& u : a.unit) u.source = 0;
        for (Unit& u : b.unit) u.source = 0;
        return a == b;
    }
    // The vertex field a byte of the packed vertex belongs to, for logs.
    const char* fieldAt(unsigned byte) const;
};
static_assert(sizeof(GxMeshLayout) == 5 + 3 * sizeof(GxMeshLayout::Unit), "operator== compares bytes");
// What the shader reads of a packed vertex: an input it takes two floats of has a zero third, a field the mesh doesn't store
// is zero, and the slot's fixed value stands where none is stored.
GxMeshVertex gxUnpackMeshVertex(const GxMeshLayout& layout, const std::uint8_t* packed);
// A layout's vertices made from decoded ones, each field's copy worked out once for a run of them. Every byte is written
// but the slot's (a refresh may land on memory that holds anything), which the caller sets.
class GxMeshPacker {
public:
    explicit GxMeshPacker(const GxMeshLayout& layout);
    void pack(const GxVertex* v, std::size_t count, std::uint8_t* out) const;

private:
    struct Copy {
        std::uint16_t from;  // where in a GxVertex
        std::uint8_t to, size;
        bool one;  // a float one follows the size's bytes
    };
    Copy copies_[2 + CombinerProgram::MaxUnits + 1];
    unsigned count_ = 0, stride_;
};
struct GxMesh {
    GxMeshLayout layout;
    std::vector<std::uint8_t> vertices;  // layout.stride bytes each
    std::vector<std::uint16_t> indices;  // triangles; GX's culling is left to the PICA
    std::uint16_t slots = 0;             // per-vertex matrix slots named (bit per slot)
    // Draws of its indices: lit, each names at most nine slots (the shader's rows), its compact slots' real ones.
    static constexpr unsigned MaxLitSlots = 9;
    // A lit, skinned mesh's texgen may take its matrix from each vertex's slot: the slot's modelview, or the texture
    // matrix paired with it (30 rows on), whose rows follow the slot's own (a slot's rows: 6 + 3 per paired unit).
    enum SlotTexgen : std::uint8_t { NoSlotTexgen, SlotModelView, SlotPaired };
    struct Group {
        std::uint32_t firstIndex = 0, indexCount = 0;
        std::uint8_t slotMap[MaxLitSlots] = {}, slotCount = 0;
        std::uint8_t slotTexgen[CombinerProgram::MaxUnits] = {}, slotRows = 6;
    };
    std::vector<Group> groups;
    std::vector<std::uint32_t> origin;  // each vertex's place among the list's own vertices (a group's copies share one)
    const std::uint8_t* arraysLo = nullptr;  // the array elements its vertices read, [lo, hi)
    const std::uint8_t* arraysHi = nullptr;
    std::uint8_t formats = 0;  // the vertex formats its primitives use, a bit each
    std::size_t vertexCount() const { return vertices.size() / layout.stride; }
    GxMeshVertex vertex(std::size_t i) const { return gxUnpackMeshVertex(layout, vertices.data() + i * layout.stride); }
};
// What one draw of a mesh needs besides its vertices: each named slot's whole transform (model space to what the
// PICA takes), each texgen's three rows and whether it divides by q, and the color when it isn't per vertex.
struct GxMeshUniforms {
    float clip[10][16];
    std::uint16_t slots = 0;
    float texgen[CombinerProgram::MaxUnits][3][4];
    // Lit: channel 0's color and alpha worked out on the GPU. View to clip, shared; per compact slot the modelview and
    // normal rows (their clip rows unused); per
    // light entry (up to four) its color (0-1, zero in a part it doesn't light); position prescaled, a0; direction,
    // a1; a2, k0, k1, k2 (one and zeros unattenuated); -prescale, 1/prescale (a far light's position scaled to about
    // one); spot (else specular), diffuse, the cosine's floor (0 clamped, -2 signed).
    bool lit = false, rgbLit = false, alphaLit = false, rgbAmbientFromVertex = false, alphaAmbientFromVertex = false;
    unsigned lights = 0;
    float light[4][6][4];
    float toClip[16];
    float modelView[GxMesh::MaxLitSlots][12], normal[GxMesh::MaxLitSlots][9];
    // A texgen through its vertex's slot (GxMesh::SlotTexgen): its rows' place after the slot's first, and each slot's
    // paired texture matrices; its texgen rows are then the dual transform.
    std::uint8_t slotTexgen[CombinerProgram::MaxUnits] = {}, slotRows = 6;
    float slotTexgenRow[CombinerProgram::MaxUnits] = {};
    float paired[GxMesh::MaxLitSlots][2][12];
    float ambient[4] = {};
    bool projective[CombinerProgram::MaxUnits] = {};
    unsigned units = 0;
    // Unlit color: per vertex, or this color for the draw; with vertex colors, each part from the vertex or from it.
    bool fixedColor = false, rgbFromVertex = true, alphaFromVertex = true;
    std::uint8_t color[4] = {};
    std::uint8_t reject = 0;  // GEN_MODE's: 1 culls what GX_CULL_BACK names, 2 GX_CULL_FRONT, 3 all
};

// Turns primitives into vertices for the PICA, transformed through XF on the CPU, and the triangles that survive GX's
// culling as indices. What it learns from the processor (vertex layouts, each matrix slot's whole transform) it keeps
// until the matching change count moves.
class GxVertexPipeline {
public:
    // Readies a primitive's kind and format under `state`; false (and `unsupported` set) when it can't be drawn.
    bool prepare(const GxCommandProcessor& gpu, const GxDrawState& state, const GxFrame& frame, std::uint8_t kind, std::uint8_t format);
    // The prepared primitive's vertices into `vertices` (verticesWritten of them, at most maxVertices), and its triangles
    // appended to `indices` as vertex numbers from `first`: how many indices that wrote, at most maxIndices. Lines and
    // points become quads, as wide as GX draws them in EFB pixels.
    std::size_t run(const std::uint8_t* data, std::uint16_t count, GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices);
    // The same from vertices read already (a small display list decoded once).
    std::size_t runDecoded(const GxVertex* decoded, std::uint16_t count, GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices);
    // Another pass over the primitive `program` just ran for (from data, or decoded): its position and color as `program`
    // gave them, only the texture coordinates worked out. False where that doesn't hold (lines, points, an atlas).
    bool runPass(const std::uint8_t* data, const GxVertex* decoded, std::uint16_t count, const GxDrawVertex* program, GxDrawVertex* vertices,
                 std::uint16_t first, std::uint16_t* indices, std::size_t& written);
    static constexpr std::size_t maxIndices(std::size_t count) { return 3 * count; }
    static constexpr std::size_t maxVertices(std::uint8_t kind, std::size_t count) { return kind >= 0xA8 ? 4 * count : count; }
    static constexpr std::size_t maxIndices(std::uint8_t kind, std::size_t count) { return kind >= 0xA8 ? 6 * count : 3 * count; }
    std::uint16_t verticesWritten = 0;
    // The state passed to prepare was read again: the next prepare reads everything.
    void stateChanged() { prepared_ = false; }
    // Where the palette unit samples for the next primitives (GX texture coordinates).
    void setPaletteTexel(float s, float t) { palette_[0] = s, palette_[1] = t; }
    // takeAtlasOutside: whether a vertex since the last call left its texture, where the atlas would sample a neighbor
    // instead of clamping.
    void setAtlas(unsigned unit, const float place[4]) { atlasUnit_ = static_cast<int>(unit), std::memcpy(atlasPlace_, place, sizeof atlasPlace_); }
    void clearAtlas() { atlasUnit_ = -1; }
    bool takeAtlasOutside() {
        const bool outside = atlasOutside_;
        atlasOutside_ = false;
        return outside;
    }
    // After prepare: whether display lists under this state can take the GPU path (unlit or riding color, folded
    // texgens with a matrix for the whole primitive, no depth texture), and the mesh a list decodes to (false when it
    // holds anything but primitives, or a matrix index that doesn't name a whole slot of the ten).
    // Why a mesh can't be used (0 when it can): the state (1 unsupported, 2 depth texture, 3 lit with C0 riding in the
    // vertex color, 4 more than four light entries, 5 a texgen's matrix chosen per vertex, other than its slot's
    // modelview or paired matrix on a lit, skinned list, 11 a texgen normalized before its second matrix) or the list (6 a
    // command other than a primitive, 7 a matrix index naming no whole slot, 8 more vertices than 16-bit indices reach, 9
    // nothing drawn), or the renderer's choice (10 small enough to join the CPU path's batch, 12 no mesh memory left
    // beside the last two frames' meshes, 13 a moving mesh's refresh finding no room in the frame's arena), or 14 a state
    // with a pass whose units differ from the program's, whose vertices only the CPU path makes again.
    enum MeshRefusal : unsigned { MeshOk, MeshUnsupported, MeshDepthTexture, MeshLitColor0, MeshLights, MeshTexgen, MeshCommand,
                                  MeshMatrixIndex, MeshTooLarge, MeshEmpty, MeshSmall, MeshNormalize, MeshBudget, MeshMoving, MeshPasses, MeshRefusals };
    unsigned meshEligible() const;
    unsigned buildMesh(const std::uint8_t* list, std::uint32_t size, GxMesh& mesh) const;
    // A built mesh's vertices decoded again from the list's arrays as they are now (where they moved), packed as `layout`
    // into `out` by its origin, each keeping its built slot: false if the list no longer gives that many vertices.
    bool refreshMesh(const std::uint8_t* list, std::uint32_t size, const GxMeshLayout& layout, const std::uint32_t* origin, const std::uint8_t* built,
                     std::uint8_t* out, std::size_t count) const;
    // The array memory a list's primitives index, [lo, hi), without decoding them: false for a list buildMesh refuses
    // as holding something other than primitives.
    bool meshSpan(const std::uint8_t* list, std::uint32_t size, const std::uint8_t*& lo, const std::uint8_t*& hi) const;
    // `layout` is the mesh's: a texgen input held as two floats has its third one folded into the rows.
    void meshUniforms(const GxMeshLayout& layout, std::uint16_t slots, const GxMesh::Group& group, GxMeshUniforms& out);
    // Another group of the same mesh: only its slots' rows.
    void meshSlotUniforms(const GxMesh::Group& group, GxMeshUniforms& out);
    bool meshLit() const { return lit_; }
    // What the lighting rows and the view-to-clip rows of a mesh's uniforms are made from changes only when these do: a draw
    // that finds them as its last one's has those rows set already.
    std::uint32_t lightingEpoch() const { return lightingEpoch_; }
    std::uint32_t toClipEpoch() const { return toClipEpoch_; }
    // The texgens' sources as a mesh's vertices hold them (row and input form per unit), for a mesh's key.
    std::uint32_t meshSources() const;
    // How this state's meshes pack their vertices (determined by the mesh's key and whether it has per-vertex matrices).
    GxMeshLayout meshLayout() const;
    // Whether this state's color comes from each vertex (otherwise GxMeshUniforms' fixed color): part of a mesh's key.
    bool meshColorFromVertex() const;
    const char* unsupported = nullptr;
    // Prepares that read the state whole, folds made, and lighting read (for the perf line).
    std::uint32_t fullPrepares = 0, lightingReads = 0;
    mutable std::uint32_t foldsMade = 0;
    // Unfolded texgens' vertices (for the perf line): a per-vertex matrix that is the position's, the texture slot paired
    // with it (30 rows on), another, or the previous vertex's again; normalized.
    mutable std::uint32_t texMatrixShared = 0, texMatrixPaired = 0, texMatrixOwn = 0, texMatrixRepeated = 0, texgensNormalized = 0;
    mutable std::uint32_t lastTexMatrix = 0;

private:
    struct TexGen {
        std::uint8_t source = 0, coord = 0, rows = 2;
        bool threeInputs = false, perVertexMatrix = false, dual = false, normalize = false;
        bool palette = false;  // the unit samples the C1 palette (setPaletteTexel), not a texgen
        std::uint32_t matrix = 0, post = 0;
        std::uint32_t ctrl = 0, postCtrl = 0;  // its XF texgen and post-texgen registers
        // With one matrix for the whole primitive and no normalizing, the texgen is one affine map: the matrix and the
        // dual transform folded together (the input's fourth component is always one).
        bool folded = false, planar = false;  // planar: two inputs, two outputs, nothing to divide by
        float fold[3][4] = {};
        const void* foldOf = nullptr;  // the Fold (and its version) that `fold` was copied from, with `planar` made of it
        std::uint32_t foldAt = 0;
        // Normalized by the dual transform, with one matrix for the primitive: its matrices are read as floats (in folds_).
        bool cached = false;
        std::uint16_t input = 0;  // where its source vector sits in a GxVertex
    };
    // What a coordinate's fold was made from (and, normalized, its float matrices; post: exactly the identity). By coordinate:
    // a program's units and its remade pass's take turns on the same slots, which would fold again at every prepare.
    struct Fold {
        std::uint32_t source_[32] = {}, sourceWords = 0, foldXf = ~0u;
        std::uint32_t rowChanges[6] = {};  // its matrix rows', then its post matrix rows' change counts (xfRowChanges)
        float fold[3][4] = {};
        std::uint32_t version = 0;  // changes with `fold`
        float matrix_[3][4] = {}, post_[3][4] = {};
        bool postIdentity = false;
        std::uint32_t matricesXf = ~0u;  // the XF change count the matrices were read at
    };
    const float* clipMatrix(std::uint32_t index) { return index < 64 && clipReady_ >> index & 1 ? clip_[index] : makeClipMatrix(index); }
    const float* makeClipMatrix(std::uint32_t index);
    bool foldStillHolds(Fold& f, const TexGen& t, std::uint32_t ctrl, std::uint32_t post) const;
    void refold(TexGen& t) const;
    void loadMatrices(Fold& f, const TexGen& t) const;
    // Lighting read again only when an XF register or a light changed (matrix loads leave it as it was).
    void readLighting(const GxCommandProcessor& gpu) {
        lighting_.read(gpu), ++lightingReads, ++lightingEpoch_, packLights();
        lightingRegisters_ = gpu.xfRegisterChanges, lightingLights_ = gpu.xfLightChanges;
    }
    // The light entries of what was read (lightEntries' count: five when more than four are lit) and their rows for the shader.
    void packLights();
    void vertex(const GxVertex& v, GxDrawVertex& d, const float* fixedClip);
    std::size_t triangles(GxDrawVertex* vertices, std::uint16_t count, std::uint16_t first, std::uint16_t* indices) const;
    // Lines and points from their vertices, transformed into the last `count` of maxVertices, as quads from the first.
    std::size_t widen(GxDrawVertex* vertices, std::uint16_t count, std::uint16_t first, std::uint16_t* indices);
    void texgen(const TexGen& t, const GxVertex& v, float st[2]) const;
    void texgenNormalized(const TexGen& t, const GxVertex& v, float st[2]) const;
    float matrix(std::uint32_t row, int column) const;

    // What the last prepare saw: a primitive with nothing changed since skips it (most of a model's do).
    bool prepared_ = false;
    std::uint8_t preparedFormat_ = 0;
    std::uint32_t preparedBp_ = 0, preparedXf_ = 0, preparedDescriptor_ = 0, preparedBeside_ = 0, preparedXfRegisters_ = 0;
    GxFrame preparedFrame_;
    const GxCommandProcessor* gpu_ = nullptr;
    const GxDrawState* state_ = nullptr;
    std::uint8_t kind_ = 0, reject_ = 0, units_ = 0;
    // The attributes a vertex is read for (GxVertexLayout::Read...): what run() needs, and what a pass's texgens need.
    static constexpr unsigned ReadChunk = 16;  // vertices decoded together, an attribute at a time
    std::uint16_t readMask_ = GxVertexLayout::ReadAll, sourceMask_ = GxVertexLayout::ReadAll;
    bool perVertexMatrix_ = false, channel_ = false, lit_ = false, fixedDepth_ = false, color0InVertex_ = false;
    std::uint8_t color0_[4] = {};
    float palette_[2] = {};
    int atlasUnit_ = -1;
    float atlasPlace_[4] = {};
    bool atlasOutside_ = false;
    std::uint32_t defaultMatrix_ = 0;
    float depth01_ = 0;
    TexGen texgens_[CombinerProgram::MaxUnits];
    mutable Fold folds_[8];
    const GxCommandProcessor* foldsFor_ = nullptr;
    // From view space to what the PICA takes, and each matrix slot's position matrix through it, while XF and the frame stay.
    std::uint32_t xfSeen_ = ~0u, viewSeen_[13] = {};  // XF 0x101A-0x1026: the viewport and projection
    GxFrame frameSeen_{0, 0, false};
    double toClip_[4][4] = {};
    float toClipFloat_[4][4] = {};  // for the CPU path's clip matrices, in the precision the shader's are
    float clip_[64][16] = {}, clipScratch_[16] = {};
    std::uint64_t clipReady_ = 0;
    GxLighting lighting_;
    std::uint32_t lightingRegisters_ = ~0u, lightingLights_ = ~0u;  // the XF register and light changes it was read at
    unsigned lightCount_ = 0;
    float lightRows_[4][6][4] = {}, ambientRow_[4] = {};
    std::uint32_t lightingEpoch_ = 0, toClipEpoch_ = 0;
};

// gx.v.pica's transform path in C++, for checking meshes against the CPU path: one vertex under a draw's uniforms, its
// clip-space position, color (0-1) and each unit's texture coordinates.
void gxMeshModel(const GxMeshUniforms& u, const GxMeshVertex& v, float clip[4], float color[4], float tex[CombinerProgram::MaxUnits][2]);

// GX's lighting for color channel 0 at one vertex, straight from the processor's state (GxLighting, read and applied).
void gxLitChannel(const GxCommandProcessor& gpu, const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]);

// The state and one primitive's triangles at once (a fresh pipeline each call).
void buildGxDraw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices,
                 const GxFrame& frame, GxDraw& out);
} // namespace smg
