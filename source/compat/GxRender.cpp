// The Wii GPU's drawing on the 3DS. Each primitive the command processor hands over becomes triangles transformed on
// the CPU (smg::buildGxDraw) and drawn with citro3d, the TEV program as combiner stages and the textures fetched from
// the Wii's memory. An EFB copy to the display ends the frame. What this can't draw yet stops by name.
#include <3ds.h>
#include <citro3d.h>
#include <GxShader.hpp>
#include <private/pe_reg.h>
#include <private/tev_reg.h>
#include <smg/FreeStore.hpp>
#include <smg/GxFifoWrite.h>
#include <smg/GxCopy.hpp>
#include <smg/GxDraw.hpp>
#include <smg/GxFog.hpp>
#include <smg/GxRender.hpp>
#include <smg/GxZones.hpp>
#include <smg/Pica.hpp>
#include <smg/Texture.hpp>
#include <smg/Thp.hpp>
#include <revolution/os.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <malloc.h>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void smgHeartbeat();          // source/compat/GameScene.cpp
extern "C" void smgDevWarpStep();       // source/compat/GameScene.cpp
extern "C" unsigned smgMovieDecodes();  // source/compat/Thp.cpp
extern "C" void smgCpuWaits(unsigned long long* game, unsigned long long* others);  // source/compat/Os.cpp
extern "C" unsigned smgFifoWrites;       // source/compat/GxFifo.cpp
extern "C" int smgDevGxDump();           // source/compat/Kpad.cpp
extern "C" int smgDevGxNull();
extern "C" int smgDevGxTiming();
extern "C" int smgDevGxCheck();
extern "C" int smgDevGxNoRaw();
extern "C" int smgDevGxZones();
extern "C" void (*smgGpuMemoryChanged)(const void* start, u32 size);  // source/compat/Os.cpp
extern std::atomic<u32> smgDspTicks;                                  // source/compat/WiiAudio.cpp

extern std::atomic<bool> smgAvSyncFramePending;  // source/compat/WiiAudio.cpp, dev.av_sync
u64 smgAvSyncMarkTick();
namespace smg {
namespace {
constexpr std::size_t VertexBytes = 2u << 20, CommandBytes = 1u << 20;
// Triangles are drawn by 16-bit indices from the buffer's start, so it holds no more vertices than they can number.
constexpr std::size_t MaxVertices = VertexBytes / sizeof(GxDrawVertex), MaxIndices = GxVertexPipeline::maxIndices(MaxVertices);
static_assert(MaxVertices <= 0x10000, "vertex numbers are 16 bits");
static_assert(sizeof(C3D_TexEnv) % 4 == 0 && sizeof(C3D_Tex) % 4 == 0, "compared a word at a time");
// Room a draw's commands need at most, in words: when less is left, the frame is ended and begun again (see roomFor).
constexpr u32 DrawCommandWords = 0x800;
// A mesh group's slot rows (up to forty uniforms, four words each) and its draw, beyond the first group's.
constexpr u32 GroupCommandWords = 0x140;

// libctru's command list written in place, where roomFor has made room: a register's first value, its header (register,
// byte mask, count less one, bit 31 for consecutive registers), its other values, and a zero to keep 8-byte alignment.
// A header counts at most 256 values, so longer writes are split as GPUCMD_Add splits them.
inline u32* putCommand(u32* at, u32 header, const u32* values, u32 count) {
    while (count) {
        const u32 part = std::min<u32>(count, 256);
        at[0] = values[0];
        at[1] = header | (part - 1) << 20;
        for (u32 i = 1; i < part; ++i) at[1 + i] = values[i];
        at += 1 + part;
        if (!(part & 1)) *at++ = 0;
        if (header & 0x80000000u) header += part;  // consecutive registers go on from the next one
        values += part, count -= part;
    }
    return at;
}
inline u32* putCommand(u32* at, u32 header, u32 value) { return putCommand(at, header, &value, 1); }
// The render target read back whole for a texture copy, still tiled the screen's way round but linear.
constexpr u32 ReadbackFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                              GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                              GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
constexpr u32 DisplayTransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                                     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
// The target's rows of 8x8 tiles (240 RGBA8 pixels wide) into a 256-wide texture's, in the transfer's 16-byte units.
constexpr u32 TargetTileRow = 240 * 8 * 4, ScratchTileRow = 256 * 8 * 4;

bool sCopiesStayOnGpu = false;
bool sStepTiming = false;  // dev.gx_timing, read when the renderer is made

// GameSystem::frameLoop's phases (smgFramePhase), timed on the game's thread and read by the perf line on the renderer's:
// drawing, the logic step, animation, waiting for the GPU's draw done (endFrame), the retrace wait.
constexpr int Phases = 5;
std::atomic<u32> sPhaseTicks[Phases] = {}, sPhaseFrames{0};

// dev.gx_zones: the render thread's zone, read every 200 us from the system core.
std::atomic<u32> sZoneSamples[ZoneCount] = {};
void sampleZones(void*) {
    for (;;) {
        svcSleepThread(200000);
        const unsigned zone = gxZone;
        if (zone < ZoneCount) sZoneSamples[zone].fetch_add(1, std::memory_order_relaxed);
    }
}

// Which 4 KiB pages some mesh was made from, one bit each over the address space: read without a lock by the flush
// hook, so the game's many flushes of other memory (matrices every frame) cost one load. Set as meshes are made,
// cleared when all are dropped; a stale bit only queues a range for nothing.
std::atomic<std::uint32_t> sMeshPageBits[1u << 15];

extern "C" u32 __ctru_heap_size;  // libctru: the free store's size

// Ranges the game flushed over mesh pages (gxRenderMemoryChanged), kept until the renderer next looks: the hook runs on
// any thread, the renderer only under the FIFO's lock. A leaf lock; past the queue's end every mesh is dropped.
struct ChangedMemory {
    LightLock lock = 1;
    std::uintptr_t start[1024], end[1024];
    unsigned count = 0;
    bool overflowed = false;
    std::atomic<bool> pending{false};  // set under the lock with each change, so the renderer can look without it
} sChanged;

// What a texture read of each EFB copy format (the PE's numbering) names: one channel reads as I4/I8, two as IA4/IA8.
std::uint8_t copyReadFormat(std::uint8_t copy) {
    switch (copy) {
    case 0: case 2: case 3: case 4: case 5: case 6: return copy;
    default: return 1;
    }
}

unsigned textureSide(float texels) {
    unsigned side = 8;
    while (side < texels && side < 1024) side <<= 1;
    return side;
}

GPU_TEXTURE_WRAP_PARAM wrapMode(unsigned gx) { return gx == 1 ? GPU_REPEAT : gx == 2 ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE; }

constexpr unsigned morton(unsigned x, unsigned y) { return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3; }

// Each slot of a C1 palette: its texel's place in the PICA's tiled order (rows from the last up), and its center as texture coordinates.
struct PaletteTexel {
    std::uint8_t offset;
    float s, t;
};
struct PaletteTexels {
    PaletteTexel slot[64] = {};
    constexpr PaletteTexels() {
        for (unsigned i = 0; i < 64; ++i) {
            const unsigned x = i % 8, row = i / 8, y = 7 - row;
            slot[i] = {static_cast<std::uint8_t>(morton(x, y)), (static_cast<float>(x) + 0.5f) / 8, (static_cast<float>(row) + 0.5f) / 8};
        }
    }
};
constexpr PaletteTexels sPaletteTexels;

[[noreturn]] void stop(const char* what) {
    std::fprintf(stderr, "The Wii GPU's drawing: %s is not the port's yet\n", what);
    std::abort();
}

struct Renderer {
    Renderer() { sStepTiming = smgDevGxTiming() != 0; }
    bool ready = false, inFrame = false, clearPending = true;
    u32 clearColor = 0x000000FF, clearDepth = 0;
    C3D_RenderTarget* target = nullptr;
    DVLB_s* shader = nullptr;
    shaderProgram_s program;
    GxDrawVertex* vertices = nullptr;
    // Above the vertices in the same block: C3D_DrawElements takes indices only above the attribute buffers' base.
    std::uint16_t* indices = nullptr;
    std::size_t used = 0, flushed = 0, indexUsed = 0, indexFlushed = 0, batchStart = 0;
    GxVertexPipeline pipeline;
    // The state the pending primitives share, built again only when the BP registers a draw reads change; consecutive
    // primitives under one state are one draw (flushBatch). It lives in the state cache (readState).
    GxDrawState* state_ = nullptr;
    // The pixel state to send: the state's own in its cache entry, or ownPixel when this draw changes it (keepDstAlpha).
    PicaPixel ownPixel;
    const PicaPixel* pixelNow = &ownPixel;
    int writeMask = 0, scissor[4] = {};
    bool stateValid = false, applied = false, scissorEmpty = false, scissorWhole = true;
    u32 stateBp = 0, stateBesideColors = 0, stateColor1 = 0, stateVersion = 0, appliedVersion = 0;
    GxFrame stateFrame;
    // The target's color and depth read back (both four bytes a pixel, the screen's way round), until drawn on again.
    u32* readback[2] = {};
    bool fresh[3] = {};  // [2]: the depth tile last read back (freshTile) still holds
    unsigned freshTile = 0;
    // A texture by where and how GX fetches it.
    using Key = std::tuple<u32, u16, u16, u8, u8, u8, u8, u8, u8, u8>;  // ..., min and mag filters, max LOD and LOD bias
    std::map<Key, C3D_Tex> textures;
    // EFB copies kept on the PICA (gpuCopy), by the Wii address they were copied to: RGBA8 at about the screen's
    // resolution, holding what a read in `readFormat` would give.
    struct GpuCopy {
        C3D_Tex texture{};
        C3D_RenderTarget* target = nullptr;
        unsigned width = 0, height = 0;
        std::uint8_t readFormat = 0;
    };
    std::map<u32, GpuCopy> gpuCopies;
    C3D_Tex scratch{};  // the target taken whole as a texture
    bool scratchReady = false;
    // The first GPU copy of each format and size is checked against the CPU's copy of the same pixels (noteGpuCopy).
    std::vector<u64> checkedKinds;
    // A THP movie's frames as RGB565, two textures taken in turn so a new frame never overwrites one the GPU may still draw.
    struct MovieFrame {
        C3D_Tex texture{};
        const std::uint8_t* key = nullptr;
        unsigned generation = 0;
        bool ready = false, topFirst = false;  // topFirst: Y2R's rows, which go in from the top
    } movie[2];
    bool y2rTried = false, y2rOpen = false;
    Handle y2rDone = 0;
    GxFrame frame{640, 480, true};
    bool indirectNoted = false, dstAlphaNoted = false, fogRangeNoted = false;
    // dev.gx_dump: one frame's draws, copies and states listed in the log every minute of the game's clock.
    const GxCommandProcessor* lastGpu = nullptr;
    // The GPU path (gx.v.pica with `transform` on): display lists of primitives kept as meshes in linear memory,
    // each drawn whole with its matrix slots and texgen rows as uniforms.
    int clipUniform = -1, texrowUniform = -1, transformUniform = -1, projectUniform[3] = {-1, -1, -1};
    int materialUniform = -1, rgbRegUniform = -1, alphaRegUniform = -1;
    int ambientUniform = -1, lightUniform = -1, lightLoopUniform = -1, litUniform = -1, rgbLitUniform = -1, alphaLitUniform = -1, anyLightUniform = -1;
    int slotRowUniform = -1, slotGenUniform[3] = {-1, -1, -1}, ambientRgbUniform = -1, ambientAlphaUniform = -1;
    struct CachedMesh {
        std::uint64_t hash = 0;  // of its key, where it is in `meshes`
        std::vector<u32> key;
        std::vector<std::pair<std::uint8_t, u32>> depends;  // the CP registers it was decoded under, and their values
        void* block = nullptr;  // vertices, then indices
        void* vertices = nullptr;  // the vertices drawn now: block's own, or a moving mesh's latest refresh
        // A mesh whose arrays move (double-buffered deformed vertices, and instances of one list each with their own) is
        // decoded again whenever they move and once a frame, into the frame's refresh arena; origin maps each vertex to
        // the list's.
        bool moving = false;
        u32 refreshedFrame = ~0u;
        std::vector<std::uint32_t> origin;
        GxMeshLayout layout;
        int loaders = 0;  // its place in meshLoaders
        u32 vertexCount = 0, indexCount = 0;
        u32 vertexPhysical = 0, indexPhysical = 0;
        float center[3] = {};  // of its positions' bounds, where its texgens are kept small
        std::uint16_t slots = 0;
        std::vector<GxMesh::Group> groups;
        u32 lastFrame = 0;  // framesShown when last drawn
        std::uintptr_t range[2][2] = {};  // the memory it was made from, [lo, hi): the display list, the array elements
    };
    std::unordered_map<std::uint64_t, CachedMesh> meshes;
    // The mesh last found or made for each slot of the key hash, looked at before the map (whose lookup divides); dropMesh clears its slot.
    static constexpr u32 MeshSlots = 1024;
    CachedMesh* meshIndex[MeshSlots] = {};
    // Lists refused for want of mesh memory, by key hash, with their mesh's size: room is looked for before building again,
    // as building is the cost.
    std::unordered_map<std::uint64_t, std::size_t> budgetRefused;
    std::unordered_set<std::uintptr_t> meshPages, texturePages;  // 4 KiB pages some mesh or texture was made from
    std::size_t meshBytes = 0;
    bool dumping = false;
    std::uint32_t gpuXf[5] = {};  // channels, color/alpha control, material, ambient as the state was built
    unsigned dumpIndex = 0;
    u64 nextDump = 0;
    // Per-frame costs, logged every few seconds (perfLine): what each optimization is measured by.
    struct Perf {
        u64 since = 0, syncTicks = 0;
        u32 frames = 0, primitives = 0, vertices = 0, triangles = 0, draws = 0, sameState = 0, color0Rides = 0;
        u32 meshDraws = 0, meshTriangles = 0, meshesMade = 0, citroDraws = 0;
        u32 pendingWhy[10] = {};  // mesh draws through citro3d after each kind of change (Pending)
        u32 meshInvalidated = 0, meshOverflows = 0, meshBudgetDrops = 0, meshMismatches = 0, flushesQueued = 0, texturesInvalidated = 0;
        u32 meshesEvicted = 0, meshesDroppedForRoom = 0, leftOutDraws = 0, passDraws = 0, meshesRefreshed = 0, meshVerticesRefreshed = 0;
        u32 mismatchWhy[5] = {};  // the key's words, the descriptor (CP 0x50/0x60), a format table, an array base, a stride
        u64 timed[6] = {};  // dev.gx_timing: CPU-path primitives (immediate, in lists), display lists (mesh draws), texture copies, movie, frame ends
        u64 cpuCopyTicks = 0;  // of the texture copies, those read back and encoded on the CPU
        // dev.gx_timing: building the draw state, applying it, preparing the vertex pipeline; of the first, reading GX's
        // state, the per-register stats, the pixel state; the CPU path's vertices and draws; a mesh's lookup, uniforms, draw;
        // a texture's first use, decoded and converted for the PICA.
        u64 stepTicks[13] = {};
        u32 steps[13] = {};
        u32 meshVerticesMade = 0;
        u32 stateHits = 0, stateMisses = 0, stateClears = 0;
        u32 cpuCopies = 0, gpuCopies = 0;
        u32 peeks = 0, peeksDeferred = 0;
        u32 atlasJoins = 0, atlasesBuilt = 0, atlasTurns = 0;
        u64 peekTicks = 0;  // depth peeks: the frame paused, the GPU waited for, one tile read back
        u32 meshesChecked = 0, meshesDiffering = 0, refreshesChecked = 0, refreshesDiffering = 0;  // dev.gx_check
        std::pair<const char*, u32> approximated[8] = {};  // new draw states whose combiner program is approximated, by reason
        u32 listVertices = 0;
        u32 refused[GxVertexPipeline::MeshRefusals] = {}, refusedOpcode[0x100] = {};
        u32 refusedVertices[GxVertexPipeline::MeshRefusals] = {};  // drawn on the CPU path inside lists refused for each reason
        u32 changed[0x100] = {};  // per BP register: state rebuilds it took part in
        u32 compilesBefore = gxTevCompiles(), moviesBefore = smgMovieDecodes(), listsBefore = 0, listBytesBefore = 0, writesBefore = smgFifoWrites;
    } perf;
    // The BP registers a draw reads and the TEV color and konst words, as of the last state built.
    u32 bpAtBuild[0x100] = {}, colorAtBuild[8] = {}, konstAtBuild[8] = {};

    // Whether C0 is all that changed since the state was built (BP 0xE0-0xE7 hold either kind of color: by word).
    bool onlyColor0Changed(const GxCommandProcessor& gpu) const {
        static const std::vector<std::uint8_t> addresses = [] {
            std::vector<std::uint8_t> list;
            for (u32 a = 0; a < 0x100; ++a)
                if (GxCommandProcessor::drawsRead(a) && (a < 0xE0 || a > 0xE7)) list.push_back(static_cast<std::uint8_t>(a));
            return list;
        }();
        for (std::uint8_t a : addresses)
            if (gpu.bp[a] != bpAtBuild[a]) return false;
        for (int i = 0; i < 8; ++i)
            if (gpu.tevKonst[i] != konstAtBuild[i] || (i != 2 && i != 3 && gpu.tevColor[i] != colorAtBuild[i])) return false;
        return true;
    }

    void init() {
        C3D_Init(CommandBytes);
        target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DisplayTransferFlags);
        shader = DVLB_ParseFile(reinterpret_cast<u32*>(gxShader), sizeof gxShader);
        shaderProgramInit(&program);
        vertices = static_cast<GxDrawVertex*>(linearAlloc(VertexBytes + MaxIndices * sizeof(std::uint16_t)));
        indices = reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(vertices) + VertexBytes);
        if (!target || !shader || R_FAILED(shaderProgramSetVsh(&program, &shader->DVLE[0])) || !vertices) stop("setting up citro3d");
        for (C3D_Tex& palette : palettes) {
            if (!C3D_TexInit(&palette, 8, 8, GPU_RGBA8)) stop("making the C1 palettes");
            C3D_TexSetFilter(&palette, GPU_NEAREST, GPU_NEAREST);
            C3D_TexSetWrap(&palette, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        }
        clipUniform = shaderInstanceGetUniformLocation(program.vertexShader, "clip");
        texrowUniform = shaderInstanceGetUniformLocation(program.vertexShader, "texrow");
        transformUniform = shaderInstanceGetUniformLocation(program.vertexShader, "transform");
        for (int u = 0; u < 3; ++u) {
            const char name[] = {'p', 'r', 'o', 'j', 'e', 'c', 't', char('0' + u), 0};
            projectUniform[u] = shaderInstanceGetUniformLocation(program.vertexShader, name);
        }
        materialUniform = shaderInstanceGetUniformLocation(program.vertexShader, "material");
        rgbRegUniform = shaderInstanceGetUniformLocation(program.vertexShader, "rgbreg");
        alphaRegUniform = shaderInstanceGetUniformLocation(program.vertexShader, "alphareg");
        const auto find = [&](const char* name) {
            const int at = shaderInstanceGetUniformLocation(program.vertexShader, name);
            if (at < 0) stop("the GX shader's uniforms");
            return at;
        };
        ambientUniform = find("ambient"), lightUniform = find("light"), lightLoopUniform = find("lightloop"), litUniform = find("lit");
        rgbLitUniform = find("rgblit"), alphaLitUniform = find("alphalit"), anyLightUniform = find("anylight");
        ambientRgbUniform = find("ambrgb"), ambientAlphaUniform = find("ambalpha");
        slotRowUniform = find("slotrow"), slotGenUniform[0] = find("slotgen0"), slotGenUniform[1] = find("slotgen1"), slotGenUniform[2] = find("slotgen2");
        if (clipUniform < 0 || texrowUniform < 0 || transformUniform < 0 || projectUniform[2] < 0 || materialUniform < 0 || alphaRegUniform < 0)
            stop("the GX shader's uniforms");
        // Every row goes to the GPU once, so row() may skip one whose value citro3d already has.
        for (const auto& [first, count] : {std::pair{clipUniform, 58}, {texrowUniform, 9}, {materialUniform, 1}, {ambientUniform, 1}, {lightUniform, 24}, {slotRowUniform, 1}})
            for (int i = 0; i < count; ++i) C3D_FVUnifSet(GPU_VERTEX_SHADER, first + i, 0, 0, 0, 0);
        if (smgDevGxZones() && R_SUCCEEDED(APT_SetAppCpuTimeLimit(80)) && !threadCreate(sampleZones, nullptr, 0x1000, 0x18, 1, true))
            stop("starting the zone sampler");
        ready = true;
    }

    void begin() {
        if (!ready) init();
        // Only the GPU's last frame is waited for: the game paces itself by the retrace (waitForRetrace), so a vblank
        // wait here (C3D_FRAME_SYNCDRAW) cost a frame that ran long half a refresh on average. Timed apart.
        const u64 start = svcGetSystemTick();
        C3D_FrameBegin(0);
        ++framesBegun;
        freeRetired();
        perf.syncTicks += svcGetSystemTick() - start;
        C3D_FrameDrawOn(target);
        queuedCommands = clearPending;
        if (clearPending) C3D_RenderTargetClear(target, C3D_CLEAR_ALL, clearColor, clearDepth);
        fresh[0] = fresh[1] = fresh[2] = false;
        clearPending = false;
        startBuffers();
        inFrame = true;
        if (smgDevGxDump() && svcGetSystemTick() >= nextDump) {
            dumping = true, dumpIndex = 0, nextDump = svcGetSystemTick() + 60ull * SYSCLOCK_ARM11;
            std::fprintf(stderr, "gx dump: frame begins (EFB %gx%g)\n", frame.width, frame.height);
        }
    }

    // Every draw reads one buffer from its start, in the shader's layout; set once per command list.
    void startBuffers() {
        used = flushed = indexUsed = indexFlushed = batchStart = 0;
        paletteUsed = paletteFlushed = 0, batchPalette = -1;
        C3D_BindProgram(&program);
        forgetSent();
        bindCpuBuffers();
        applied = false;
    }

    // What citro3d was last given by the draws here, so a draw passes on only what changed (each citro3d setter marks
    // its part to be sent to the GPU again, a whole block of registers). Forgotten whenever other code here sets
    // citro3d's state itself (texture copies, movie frames) and at each command list.
    struct Sent {
        bool pixel = false, cull = false, scissor = false, texture[CombinerProgram::MaxUnits] = {}, fixed[7] = {};
        PicaPixel was;
        int writeMask = 0;
        GPU_CULLMODE cullMode = GPU_CULL_NONE;
        bool scissorWhole = true;
        int scissorBox[4] = {};
        C3D_Tex* bound[CombinerProgram::MaxUnits] = {};
        C3D_Tex boundWas[CombinerProgram::MaxUnits] = {};
        C3D_FVec fixedWas[7] = {};
        int meshLayout = -1;  // the mesh loaders citro3d holds (meshLoaders' place), -1 none
        bool aliasBuffers = true;  // the GPU may hold buffers past the first (a mesh's texgen reading the vertex's bytes again)
        bool fog = false;
        std::uint32_t fogWords[6] = {};
    } sent;
    void forgetSent() { sent = Sent{}, pend(PendingList); }
    // The port's own draws (copies, movies) and whatever draws after the frame take no fog; a GX draw sends its own again.
    void fogOff() { C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false), fogBits = 0, sent.fog = false; }
    // Whether citro3d holds state of ours not yet sent (one of its setters since its last draw): then a draw goes through
    // citro3d, which sends it; otherwise a mesh draw writes its few registers itself (writeMeshDraw). Every command list
    // begins pending, so its first draw is citro3d's and citro3d submits the list.
    bool pending = true;
    enum Pending : unsigned { PendingCombiner, PendingFragment, PendingCull, PendingScissor, PendingTexture, PendingFixed, PendingCpuBuffers,
                              PendingMeshLayout, PendingFog, PendingList };
    u32 pendingWhy = 1u << PendingList;
    void pend(Pending why) { pending = true, pendingWhy |= 1u << why; }
    // The blocks a mesh draw writes into the command list itself when they are all that's pending (writeChangedBlocks),
    // and what in them changed since the GPU last had them. citro3d's copies are set all the same, so its own next draw
    // sends them again and its state stays the truth.
    static constexpr u32 RawBlocks = 1u << PendingCombiner | 1u << PendingFragment | 1u << PendingCull | 1u << PendingTexture |
                                     1u << PendingFixed | 1u << PendingMeshLayout | 1u << PendingCpuBuffers;
    u32 stagesChanged = 0;  // combiner stages, and bit 6 the buffer's updates and color
    std::uint32_t fogWords[6] = {};  // the state's fog registers (GxFogRegisters), all zero with fog off
    C3D_FogLut fogLut;
    u32 fogBits = 0;  // fog's mode and z flip, which share GPUREG_TEXENV_UPDATE_BUFFER with the buffer's updates
    u32 testsChanged = 0;   // 1 the alpha test, 2 the blend, 4 the depth test and write mask
    u32 unitsChanged = 0, fixedChanged = 0;
    u32 unitsBound = 0;     // texture units citro3d has a texture for: it never unbinds one
    C3D_AttrInfo meshAttributes;
    // A mesh layout's loaders: the shader's seven inputs in a fixed order (position, color, three texgen inputs, normal, slot),
    // each read from a buffer or fixed (sendFixed numbers them so). Buffer 0 is the vertices; each texgen input that is the
    // position's or the normal's bytes read again is a buffer of its own at that field.
    static constexpr int MaxMeshBuffers = 1 + CombinerProgram::MaxUnits;
    struct MeshLoaders {
        GxMeshLayout layout;
        C3D_AttrInfo attributes;
        struct Buffer {
            u64 permutation;
            u32 offset;
            int components;
        } buffers[MaxMeshBuffers];
        int bufferCount;
    };
    std::vector<MeshLoaders> meshLoaders;
    int loadersFor(const GxMeshLayout& layout) {
        for (std::size_t i = 0; i < meshLoaders.size(); ++i)
            if (meshLoaders[i].layout.sameBuffers(layout)) return static_cast<int>(i);
        MeshLoaders m = {};
        m.layout = layout;
        C3D_AttrInfo* attributes = &m.attributes;
        AttrInfo_Init(attributes);
        MeshLoaders::Buffer& vertices = m.buffers[m.bufferCount++];
        const auto load = [&](int reg, GPU_FORMATS format, int count) {
            vertices.permutation |= u64(AttrInfo_AddLoader(attributes, reg, format, count)) << (4 * vertices.components++);
        };
        load(0, GPU_FLOAT, 3);
        if (layout.color != GxMeshLayout::Absent) load(1, GPU_UNSIGNED_BYTE, 4);
        else AttrInfo_AddFixed(attributes, 1);
        for (int unit = 0; unit < 3; ++unit) {
            const GxMeshLayout::Unit& input = layout.unit[unit];
            if (input.input == GxMeshLayout::Unused) AttrInfo_AddFixed(attributes, 2 + unit);
            else if (input.input == GxMeshLayout::Stored) load(2 + unit, GPU_FLOAT, input.floats);
            else {
                // Padded out to the stride: the PICA freezes on a loader whose attributes don't add up to it.
                MeshLoaders::Buffer& again = m.buffers[m.bufferCount++];
                again = {u64(AttrInfo_AddLoader(attributes, 2 + unit, GPU_FLOAT, input.floats)),
                         input.input == GxMeshLayout::OfPosition ? 0u : layout.normal, 1};
                for (unsigned left = layout.stride - 4u * input.floats; left;) {
                    const unsigned pad = std::min(left, 16u);
                    again.permutation |= u64(0xB + pad / 4) << (4 * again.components++);
                    left -= pad;
                }
            }
        }
        if (layout.lit) {
            load(6, GPU_FLOAT, 3);
        } else {
            AttrInfo_AddFixed(attributes, 6);
            if (layout.normal != GxMeshLayout::Absent) vertices.permutation |= u64(0xE) << (4 * vertices.components++);  // skips the 12 bytes
        }
        if (layout.slot != GxMeshLayout::Absent) load(5, GPU_FLOAT, 1);
        else AttrInfo_AddFixed(attributes, 5);
        meshLoaders.push_back(m);
        return static_cast<int>(meshLoaders.size() - 1);
    }
    // citro3d has sent everything: after one of its draws.
    void sentAll() { pending = false, pendingWhy = 0, stagesChanged = testsChanged = unitsChanged = fixedChanged = 0; }

    // A mesh draw's vertex shader uniforms, set in citro3d's copies (which then always match the GPU) and sent by citro3d
    // when it has state of ours pending anyway, else written here (sendUniforms).
    u32 rowsChanged[3] = {};  // 96 rows
    bool boolsChanged = false, intsChanged = false;
    void row(int id, float x, float y, float z, float w) {
        C3D_FVec& now = C3D_FVUnif[GPU_VERTEX_SHADER][id];
        if (now.x == x && now.y == y && now.z == z && now.w == w) return;
        now.x = x, now.y = y, now.z = z, now.w = w;
        rowsChanged[id >> 5] |= 1u << (id & 31);
    }
    void boolean(int id, bool value) {
        u16& bools = C3D_BoolUnifs[GPU_VERTEX_SHADER];
        const u16 next = value ? u16(bools | BIT(id - 0x68)) : u16(bools & ~BIT(id - 0x68));
        if (next != bools) bools = next, boolsChanged = true;
    }
    // The bools `mask` names (a bit each from id 0x68) set to `value`'s.
    void booleans(u16 mask, u16 value) {
        u16& bools = C3D_BoolUnifs[GPU_VERTEX_SHADER];
        const u16 next = u16(bools & ~mask | value);
        if (next != bools) bools = next, boolsChanged = true;
    }
    static u16 boolBit(int id) { return u16(1u << (id - 0x68)); }
    void integer(int id, int x, int y, int z, int w) {
        C3D_IVec& now = C3D_IVUnif[GPU_VERTEX_SHADER][id - 0x60];
        if (now != IVec_Pack(x, y, z, w)) now = IVec_Pack(x, y, z, w), intsChanged = true;
    }
    void sendUniforms() {
        if (pending) {
            for (int word = 0; word < 3; ++word)
                for (u32 bits = rowsChanged[word]; bits; bits &= bits - 1) C3D_FVUnifDirty[GPU_VERTEX_SHADER][word * 32 + __builtin_ctz(bits)] = true;
            if (boolsChanged) C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = true;
            if (intsChanged) C3D_IVUnifDirty[GPU_VERTEX_SHADER][lightLoopUniform - 0x60] = true;
        } else {
            u32* at = gpuCmdBuf + gpuCmdBufOffset;
            for (int word = 0; word < 3; ++word) {
                while (rowsChanged[word]) {
                    // A run of changed rows from the first left, as one upload (a word of the bits at a time).
                    const int id = word * 32 + __builtin_ctz(rowsChanged[word]);
                    int end = id;
                    while (end < C3D_FVUNIF_COUNT) {
                        const int shift = end & 31;
                        const u32 rest = rowsChanged[end >> 5] >> shift;  // the rows from `end` to the word's end
                        if (!(rest & 1)) break;
                        const int ones = rest == ~0u ? 32 : __builtin_ctz(~rest);  // how many of them in a row changed
                        rowsChanged[end >> 5] &= ~((ones == 32 ? ~0u : (1u << ones) - 1) << shift);
                        end += ones;
                        if (end & 31) break;
                    }
                    at = putCommand(at, 0x000F0000 | GPUREG_VSH_FLOATUNIFORM_CONFIG, 0x80000000u | u32(id));
                    at = putCommand(at, 0x000F0000 | GPUREG_VSH_FLOATUNIFORM_DATA, reinterpret_cast<const u32*>(&C3D_FVUnif[GPU_VERTEX_SHADER][id]),
                                    u32(end - id) * 4);
                }
            }
            if (boolsChanged) at = putCommand(at, 0x000F0000 | GPUREG_VSH_BOOLUNIFORM, 0x7FFF0000u | C3D_BoolUnifs[GPU_VERTEX_SHADER]);
            if (intsChanged)
                at = putCommand(at, 0x000F0000 | (GPUREG_VSH_INTUNIFORM_I0 + u32(lightLoopUniform - 0x60)), C3D_IVUnif[GPU_VERTEX_SHADER][lightLoopUniform - 0x60]);
            gpuCmdBufOffset = u32(at - gpuCmdBuf);
        }
        rowsChanged[0] = rowsChanged[1] = rowsChanged[2] = 0, boolsChanged = intsChanged = false;
    }

    // The pending blocks of state, written as citro3d's binders write them (C3Di_TexEnvBind, C3Di_EffectBind's registers
    // that changed, C3Di_SetTex and the texture units' status, its fixed attributes, C3Di_AttrInfoBind).
    void writeChangedBlocks() {
        u32* at = gpuCmdBuf + gpuCmdBufOffset;
        const PicaPixel& was = sent.was;
        for (unsigned i = 0; i < CombinerProgram::MaxStages; ++i)
            if (stagesChanged >> i & 1)
                at = putCommand(at, 0x800F0000 | (GPUREG_TEXENV0_SOURCE + (i < 4 ? i : i + 2) * 8), reinterpret_cast<const u32*>(&was.env[i]), 5);
        if (stagesChanged >> 6 & 1) {
            at = putCommand(at, 0x00070000 | GPUREG_TEXENV_UPDATE_BUFFER,
                            u32(was.bufferUpdate[0] & 0xF) << 8 | u32(was.bufferUpdate[1] & 0xF) << 12 | fogBits);
            at = putCommand(at, 0x000F0000 | GPUREG_TEXENV_BUFFER_COLOR, was.bufferColor);
        }
        if (testsChanged & 1) at = putCommand(at, 0x000F0000 | GPUREG_FRAGOP_ALPHA_TEST, u32(was.alphaTest) | u32(was.alphaFunc & 7) << 4 | u32(was.alphaReference) << 8);
        if (testsChanged & 2)
            at = putCommand(at, 0x000F0000 | GPUREG_BLEND_FUNC, u32(was.equation) | u32(was.alphaEquation) << 8 | u32(was.source) << 16 | u32(was.destination) << 20 |
                                                                     u32(was.alphaSource) << 24 | u32(was.alphaDestination) << 28);
        if (testsChanged & 4) {
            const u32 function = was.depthTest ? u32(was.depthFunc) & 7 : u32(GPU_ALWAYS);
            at = putCommand(at, 0x000F0000 | GPUREG_DEPTH_COLOR_MASK, u32(was.depthTest) | function << 4 | u32(sent.writeMask) << 8);
            at = putCommand(at, 0x00080000 | GPUREG_GAS_DELTAZ_DEPTH, u32(GPU_MAKEGASDEPTHFUNC(function)) << 24);
        }
        if (pendingWhy >> PendingCull & 1) at = putCommand(at, 0x000F0000 | GPUREG_FACECULLING_CONFIG, u32(sent.cullMode) & 3);
        if (unitsChanged) {
            static constexpr u32 border[3] = {GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR, GPUREG_TEXUNIT2_BORDER_COLOR};
            static constexpr u32 type[3] = {GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE, GPUREG_TEXUNIT2_TYPE};
            for (unsigned unit = 0; unit < 3; ++unit) {
                if (!(unitsChanged >> unit & 1)) continue;
                const C3D_Tex& t = *sent.bound[unit];
                const u32 registers[5] = {t.border, t.dim, t.param, t.lodParam, osConvertVirtToPhys(t.data) >> 3};
                at = putCommand(at, 0x800F0000 | border[unit], registers, 5);
                at = putCommand(at, 0x000F0000 | type[unit], u32(t.fmt));
            }
            // citro3d's texConfig (BIT(12) from C3D_Init, the bound units), then the texture cache cleared.
            at = putCommand(at, 0x000B0000 | GPUREG_TEXUNIT_CONFIG, BIT(12) | unitsBound);
            at = putCommand(at, 0x00040000 | GPUREG_TEXUNIT_CONFIG, BIT(16));
            at = putCommand(at, 0x000F0000 | GPUREG_TEXUNIT0_SHADOW, BIT(0));
        }
        for (unsigned i = 0; i < 7; ++i) {
            if (!(fixedChanged >> i & 1)) continue;
            const C3D_FVec& v = sent.fixedWas[i];
            // C3D_ImmSendAttrib's packing: four 24-bit floats, w first.
            const u32 x = f32tof24(v.x), y = f32tof24(v.y), z = f32tof24(v.z), w = f32tof24(v.w);
            const u32 packed[3] = {w << 8 | z >> 16, z << 16 | y >> 8, y << 24 | x};
            at = putCommand(at, 0x000F0000 | GPUREG_FIXEDATTRIB_INDEX, i);
            at = putCommand(at, 0x800F0000 | GPUREG_FIXEDATTRIB_DATA0, packed, 3);
        }
        if (pendingWhy >> PendingMeshLayout & 1) {
            const C3D_AttrInfo& a = meshAttributes;
            at = putCommand(at, 0x800F0000 | GPUREG_ATTRIBBUFFERS_FORMAT_LOW, a.flags, 2);
            at = putCommand(at, 0x000B0000 | GPUREG_VSH_INPUTBUFFER_CONFIG, 0xA0000000u | u32(a.attrCount - 1));
            at = putCommand(at, 0x000F0000 | GPUREG_VSH_NUM_ATTR, u32(a.attrCount - 1));
            at = putCommand(at, 0x800F0000 | GPUREG_VSH_ATTRIBUTES_PERMUTATION_LOW, reinterpret_cast<const u32*>(&a.permutation), 2);
        }
        gpuCmdBufOffset = u32(at - gpuCmdBuf);
        sentAll();
    }

    // A group's draw as C3D_DrawElements writes it for triangles of 16-bit indices, after the buffers aimed at the mesh.
    void writeMeshDraw(const CachedMesh& mesh, const GxMesh::Group& group, bool aimBuffer) {
        u32* at = gpuCmdBuf + gpuCmdBufOffset;
        if (aimBuffer) {
            constexpr u32 Base = 0x18000000;  // BufInfo_Init's, which citro3d's own binds use
            const MeshLoaders& loaders = meshLoaders[mesh.loaders];
            // Buffers past the mesh's are cleared while the GPU may hold some of an earlier mesh's (citro3d's binds clear them all).
            const int count = loaders.bufferCount > 1 || sent.aliasBuffers ? MaxMeshBuffers : 1;
            u32 buffers[3 * MaxMeshBuffers] = {};
            for (int b = 0; b < loaders.bufferCount; ++b) {
                const MeshLoaders::Buffer& buffer = loaders.buffers[b];
                buffers[3 * b] = mesh.vertexPhysical + buffer.offset - Base;
                buffers[3 * b + 1] = u32(buffer.permutation);
                buffers[3 * b + 2] = u32(buffer.permutation >> 32) | u32(mesh.layout.stride) << 16 | u32(buffer.components) << 28;
            }
            sent.aliasBuffers = loaders.bufferCount > 1;
            at = putCommand(at, 0x000F0000 | GPUREG_ATTRIBBUFFERS_LOC, Base >> 3);
            at = putCommand(at, 0x800F0000 | GPUREG_ATTRIBBUFFER0_OFFSET, buffers, 3 * count);
        }
        const u32 indices = mesh.indexPhysical + group.firstIndex * 2 - 0x18000000;
        at = putCommand(at, 0x00020000 | GPUREG_PRIMITIVE_CONFIG, 0x300);
        at = putCommand(at, 0x000F0000 | GPUREG_RESTART_PRIMITIVE, 1);
        at = putCommand(at, 0x000F0000 | GPUREG_INDEXBUFFER_CONFIG, indices | 1u << 31);
        at = putCommand(at, 0x000F0000 | GPUREG_NUMVERTICES, group.indexCount);
        at = putCommand(at, 0x000F0000 | GPUREG_VERTEX_OFFSET, 0);
        at = putCommand(at, 0x00020000 | GPUREG_GEOSTAGE_CONFIG, 0x100);
        at = putCommand(at, 0x00020000 | GPUREG_GEOSTAGE_CONFIG2, 0x100);
        at = putCommand(at, 0x00010000 | GPUREG_START_DRAW_FUNC0, 0);
        at = putCommand(at, 0x000F0000 | GPUREG_DRAWELEMENTS, 1);
        at = putCommand(at, 0x00010000 | GPUREG_START_DRAW_FUNC0, 1);
        at = putCommand(at, 0x00020000 | GPUREG_GEOSTAGE_CONFIG, 0);
        at = putCommand(at, 0x00020000 | GPUREG_GEOSTAGE_CONFIG2, 0);
        at = putCommand(at, 0x000F0000 | GPUREG_VTX_FUNC, 1);
        at = putCommand(at, 0x00080000 | GPUREG_PRIMITIVE_CONFIG, 0);
        at = putCommand(at, 0x00080000 | GPUREG_PRIMITIVE_CONFIG, 0);
        gpuCmdBufOffset = u32(at - gpuCmdBuf);
    }

    void sendPixel() {
        const PicaPixel& pixel = *pixelNow;
        const bool all = !sent.pixel;
        PicaPixel& was = sent.was;  // brought up to `pixel` a part at a time, as it is sent
        for (unsigned i = 0; i < CombinerProgram::MaxStages; ++i)
            if (all || !sameWords(&was.env[i], &pixel.env[i], sizeof(C3D_TexEnv) / 4))
                *C3D_GetTexEnv(int(i)) = pixel.env[i], was.env[i] = pixel.env[i], stagesChanged |= 1u << i, pend(PendingCombiner);
        if (all || was.bufferColor != pixel.bufferColor)
            C3D_TexEnvBufColor(pixel.bufferColor), was.bufferColor = pixel.bufferColor, stagesChanged |= 1u << 6, pend(PendingCombiner);
        if (all || was.bufferUpdate[0] != pixel.bufferUpdate[0] || was.bufferUpdate[1] != pixel.bufferUpdate[1]) {
            C3D_TexEnvBufUpdate(C3D_RGB, pixel.bufferUpdate[0]), C3D_TexEnvBufUpdate(C3D_Alpha, pixel.bufferUpdate[1]);
            was.bufferUpdate[0] = pixel.bufferUpdate[0], was.bufferUpdate[1] = pixel.bufferUpdate[1];
            stagesChanged |= 1u << 6, pend(PendingCombiner);
        }
        if (all || was.alphaTest != pixel.alphaTest || was.alphaFunc != pixel.alphaFunc || was.alphaReference != pixel.alphaReference) {
            C3D_AlphaTest(pixel.alphaTest, pixel.alphaFunc, pixel.alphaReference), testsChanged |= 1, pend(PendingFragment);
            was.alphaTest = pixel.alphaTest, was.alphaFunc = pixel.alphaFunc, was.alphaReference = pixel.alphaReference;
        }
        if (all || was.equation != pixel.equation || was.alphaEquation != pixel.alphaEquation || was.source != pixel.source ||
            was.destination != pixel.destination || was.alphaSource != pixel.alphaSource || was.alphaDestination != pixel.alphaDestination) {
            C3D_AlphaBlend(pixel.equation, pixel.alphaEquation, pixel.source, pixel.destination, pixel.alphaSource, pixel.alphaDestination);
            was.equation = pixel.equation, was.alphaEquation = pixel.alphaEquation, was.source = pixel.source, was.destination = pixel.destination;
            was.alphaSource = pixel.alphaSource, was.alphaDestination = pixel.alphaDestination;
            testsChanged |= 2, pend(PendingFragment);
        }
        if (all || was.depthTest != pixel.depthTest || was.depthFunc != pixel.depthFunc || sent.writeMask != writeMask) {
            C3D_DepthTest(pixel.depthTest, pixel.depthTest ? pixel.depthFunc : GPU_ALWAYS, static_cast<GPU_WRITEMASK>(writeMask)), testsChanged |= 4,
                pend(PendingFragment);
            was.depthTest = pixel.depthTest, was.depthFunc = pixel.depthFunc;
        }
        was.depthWrite = pixel.depthWrite, sent.writeMask = writeMask, sent.pixel = true;
    }
    // GX fog through the PICA's fog unit, its table from the GX registers (smg::picaFogTable): citro3d sends it, as its draw
    // writes the table, so a change makes the next draw citro3d's.
    void sendFog() {
        if (sent.fog && sameWords(sent.fogWords, fogWords, 6)) return;
        std::memcpy(sent.fogWords, fogWords, sizeof fogWords), sent.fog = true;
        const GxFog fog = readGxFog(fogWords);
        if (fog.type) {
            float table[256];
            picaFogTable(fog, table);
            FogLut_FromArray(&fogLut, table);
            C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, true);
            C3D_FogColor(u32(fog.color[0]) | u32(fog.color[1]) << 8 | u32(fog.color[2]) << 16);
            C3D_FogLutBind(&fogLut);
            fogBits = GPU_FOG | BIT(16);
        } else {
            C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
            fogBits = 0;
        }
        pend(PendingFog);
    }
    void sendCull(GPU_CULLMODE mode) {
        if (sent.cull && sent.cullMode == mode) return;
        C3D_CullFace(mode);
        sent.cull = true, sent.cullMode = mode, pend(PendingCull);
    }
    void sendScissor() {
        if (sent.scissor && sent.scissorWhole == scissorWhole && (scissorWhole || sameWords(sent.scissorBox, scissor, 4))) return;
        if (scissorWhole) C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        else C3D_SetScissor(GPU_SCISSOR_NORMAL, scissor[0], scissor[1], scissor[2], scissor[3]);
        sent.scissor = true, sent.scissorWhole = scissorWhole, std::memcpy(sent.scissorBox, scissor, sizeof scissor), pend(PendingScissor);
    }
    // The same texture object with the same registers needs no binding again.
    void sendTexture(unsigned unit, C3D_Tex& texture) {
        if (sent.texture[unit] && sent.bound[unit] == &texture && sameWords(&sent.boundWas[unit], &texture, sizeof texture / 4)) return;
        C3D_TexBind(int(unit), &texture);
        sent.texture[unit] = true, sent.bound[unit] = &texture, sent.boundWas[unit] = texture, pend(PendingTexture);
        unitsChanged |= 1u << unit, unitsBound |= 1u << unit;
    }
    // `attribute` is the attribute's index among the loaders and fixed ones, as AttrInfo_AddFixed returns it.
    void sendFixed(int attribute, float x, float y, float z, float w) {
        C3D_FVec& was = sent.fixedWas[attribute];
        if (sent.fixed[attribute] && was.x == x && was.y == y && was.z == z && was.w == w) return;
        C3D_FVec* value = C3D_FixedAttribGetWritePtr(attribute);
        value->x = x, value->y = y, value->z = z, value->w = w;
        sent.fixed[attribute] = true, was = *value, fixedChanged |= 1u << attribute, pend(PendingFixed);
    }

    // The CPU path's vertices: transformed already, the shader passing them on. Bound again only when a draw from them
    // follows a mesh's (consecutive meshes are most of a scene's draws).
    bool cpuBound = false;
    void ensureCpuBuffers() {
        if (!cpuBound) bindCpuBuffers();
    }
    void bindCpuBuffers() {
        cpuBound = true, pend(PendingCpuBuffers);
        sent.meshLayout = -1;
        for (bool& known : sent.fixed) known = false;  // the vertex loaders' fixed values, not trusted past another layout
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, transformUniform, false);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, rgbRegUniform, false);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, alphaRegUniform, false);
        C3D_AttrInfo* attributes = C3D_GetAttrInfo();
        AttrInfo_Init(attributes);
        AttrInfo_AddLoader(attributes, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(attributes, 1, GPU_UNSIGNED_BYTE, 4);
        for (int unit = 0; unit < 3; ++unit) AttrInfo_AddLoader(attributes, 2 + unit, GPU_FLOAT, 2);
        C3D_BufInfo* buffers = C3D_GetBufInfo();
        BufInfo_Init(buffers);
        BufInfo_Add(buffers, vertices, sizeof(GxDrawVertex), 5, 0x43210);
    }

    // The vertices and indices written since the GPU last took a command list, out of the CPU's cache (Azahar has none).
    // Meshes and textures flush their own memory when made, so a frame's end flushes only these and the command list:
    // C3D_FrameEnd(0) would flush the whole linear heap each time, which a real console pays for in cache lines.
    void flushVertices() {
        if (refreshHi > refreshLo) GSPGPU_FlushDataCache(reinterpret_cast<void*>(refreshLo), refreshHi - refreshLo);
        refreshLo = UINTPTR_MAX, refreshHi = 0;
        if (used > flushed) GSPGPU_FlushDataCache(vertices + flushed, (used - flushed) * sizeof(GxDrawVertex));
        if (indexUsed > indexFlushed) GSPGPU_FlushDataCache(indices + indexFlushed, (indexUsed - indexFlushed) * sizeof(std::uint16_t));
        flushed = used, indexFlushed = indexUsed;
        if (paletteUsed > paletteFlushed)
            for (unsigned t = paletteFlushed / PaletteSlots; t <= (paletteUsed - 1) / PaletteSlots; ++t) C3D_TexFlush(&palettes[t]);
        paletteFlushed = paletteUsed;
    }

    // citro3d's GX queue holds 32 commands until the next C3D_FrameBegin empties it, and a frame's end takes two: a frame
    // with many copies and peeks (two commands each) ends and begins again before it would overflow.
    u32 queuedCommands = 0;
    void roomInQueue(u32 commands) {
        if (inFrame && queuedCommands + commands + 2 > 32) pause(), resume();
        queuedCommands += commands;
    }

    // Textures whose memory the game rewrote, kept until the GPU is done with what was queued before (C3D_FrameBegin
    // waits for it), so the frame needn't stop for them.
    std::vector<C3D_Tex> retired;
    void freeRetired() {
        for (C3D_Tex& t : retired) C3D_TexDelete(&t);
        retired.clear();
    }

    // Ends the frame drawn so far without showing it; resume() begins again on the same target once the GPU is done, so
    // the vertex buffer can start over. Inside a frame citro3d's "sync" transfers only queue: readbacks go between.
    void pause() {
        flushBatch();
        const GxZoneScope zone(ZonePause);
        flushVertices();
        C3D_RenderTargetDetachOutput(target);
        fogOff();
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
    }
    void resume() {
        const GxZoneScope zone(ZonePause);
        C3D_FrameBegin(0);
        ++framesBegun;
        freeRetired();
        queuedCommands = 0;
        C3D_FrameDrawOn(target);
        C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DisplayTransferFlags);
        startBuffers();
    }

    C3D_Tex& texture(const GxTextureMap& map) {
        if (auto copy = gpuCopies.find(map.address); copy != gpuCopies.end()) {
            GpuCopy& g = copy->second;
            if (map.width != g.width || map.height != g.height || map.format != g.readFormat) stop("a GPU-kept EFB copy read as another texture");
            C3D_TexSetFilter(&g.texture, map.magFilter ? GPU_LINEAR : GPU_NEAREST, (map.minFilter & 4) ? GPU_LINEAR : GPU_NEAREST);
            C3D_TexSetWrap(&g.texture, wrapMode(map.wrapS), wrapMode(map.wrapT));
            return g.texture;
        }
        const Key key{map.address, map.width, map.height, map.format, map.wrapS, map.wrapT, map.minFilter, map.magFilter, map.maxLod, map.lodBias};
        if (auto found = textures.find(key); found != textures.end()) return found->second;
        if (map.format == 8 || map.format == 9 || map.format == 10) stop("a palette texture");
        const Step converting(perf, 11);
        const auto power = [](unsigned v) { return v >= 8 && v <= 1024 && !(v & (v - 1)); };
        const unsigned levels = mipLevels(key);
        if (map.format == static_cast<int>(GxFormat::I8) && power(map.width) && power(map.height) && levels == 1) {
            // Straight into LA8, without decoding to RGBA first: SnowFloor's footprints redraw theirs every frame.
            C3D_Tex& texture = textures[key];
            if (!withRoom([&] { return C3D_TexInit(&texture, map.width, map.height, GPU_LA8); })) stop("a texture the PICA takes no memory for");
            tileIntensity8(static_cast<const std::uint8_t*>(OSPhysicalToCached(map.address)), map.width, map.height,
                           static_cast<std::uint8_t*>(texture.data));
            C3D_TexFlush(&texture);
            C3D_TexSetFilter(&texture, map.magFilter ? GPU_LINEAR : GPU_NEAREST, (map.minFilter & 4) ? GPU_LINEAR : GPU_NEAREST);
            C3D_TexSetWrap(&texture, wrapMode(map.wrapS), wrapMode(map.wrapT));
            const auto [lo, hi] = textureRange(key);
            markPages(lo, hi, texturePages);
            return texture;
        }
        ModelTexture source;
        source.format = static_cast<GxFormat>(map.format);
        source.wrapS = map.wrapS, source.wrapT = map.wrapT;
        source.minFilter = (map.minFilter & 4) != 0, source.magFilter = map.magFilter;  // hardware min filters: linear in bit 2
        const std::size_t size = gxImageSize(source.format, map.width, map.height);
        std::string error;
        if (!size || !decodeGxImage({static_cast<const std::uint8_t*>(OSPhysicalToCached(map.address)), size}, source.format, map.width,
                                    map.height, {}, GxPalette::IA8, source.image, error)) {
            std::fprintf(stderr, "The Wii GPU's drawing: a texture at 0x%08lx (%ux%u, format %u): %s\n", static_cast<unsigned long>(map.address),
                         map.width, map.height, map.format, error.c_str());
            std::abort();
        }
        C3D_Tex& texture = textures[key];
        if (levels > 1) {
            // The game's own mip levels, which the PICA samples as GX does (the 3DS's smaller screen minifies more).
            std::vector<TiledTexture> chain{tile(source)};
            for (unsigned level = 1; level < levels; ++level) {
                const unsigned w = map.width >> level, h = map.height >> level;
                const std::size_t offset = gxLevelOffset(source.format, map.width, map.height, level);
                ModelTexture smaller = source;
                if (!decodeGxImage({static_cast<const std::uint8_t*>(OSPhysicalToCached(map.address)) + offset, gxImageSize(source.format, w, h)},
                                   source.format, w, h, {}, GxPalette::IA8, smaller.image, error))
                    stop("a texture's mip level that doesn't decode");
                chain.push_back(tile(smaller, chain.front().format));
            }
            const float bias = static_cast<std::int8_t>(map.lodBias) / 32.0f;
            if (!withRoom([&] { return upload(texture, chain, (map.minFilter & 3) == 2, bias); })) stop("a texture the PICA takes no memory for");
        } else {
            const TiledTexture tiled = tile(source);
            if (!withRoom([&] { return upload(texture, tiled); })) stop("a texture the PICA takes no memory for");
        }
        const auto [lo, hi] = textureRange(key);
        markPages(lo, hi, texturePages);
        return texture;
    }

    // Levels for the PICA when the game samples between them (the min filter's low bits), else the first alone.
    static unsigned mipLevels(const Key& key) {
        return std::get<6>(key) & 3 ? picaMipLevels(std::get<1>(key), std::get<2>(key), std::get<8>(key)) : 1;
    }

    // Depth from a texture: the clear's constant Z texture is the one case the PICA can follow, by vertex depth.
    void depthFromTexture(GxDrawState& draw) {
        const int stage = draw.material.tevStageCount - 1;
        const std::uint8_t map = draw.material.stages[stage].texMap;
        if (draw.depthTextureOp != 2 || draw.depthTextureType != 2 || map >= 8 || draw.textures[map].format != 6)
            stop("depth from a texture other than a replacing Z24 one");
        const GxTextureMap& t = draw.textures[map];
        const auto* texels = static_cast<const std::uint8_t*>(OSPhysicalToCached(t.address));
        // Z24X8 tiles as RGBA8 does: 4x4 texels, their A and R bytes, then their G and B bytes.
        const std::size_t tiles = std::size_t((t.width + 3) / 4) * ((t.height + 3) / 4);
        u32 depth = 0;
        for (std::size_t tile = 0; tile < tiles; ++tile) {
            for (int texel = 0; texel < 16; ++texel) {
                const std::uint8_t* ar = texels + tile * 64 + texel * 2;
                const std::uint8_t* gb = ar + 32;
                const u32 z = u32(ar[1]) << 16 | u32(gb[0]) << 8 | gb[1];
                if (tile == 0 && texel == 0) depth = z;
                else if (z != depth) stop("a Z texture that is not one value");
            }
        }
        draw.fixedDepth = true;
        draw.depth01 = static_cast<float>((depth + draw.depthTextureOffset) & 0xFFFFFF) / 16777215.0f;
    }

    void draw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* data,
              const GxVertex* decoded = nullptr) {
        ++perf.primitives, perf.vertices += count;
        ensureState(gpu, true);
        if (state_->leftOut) {
            static bool noted = false;
            if (!noted) std::fprintf(stderr, "The Wii GPU's drawing: %s is left out\n", state_->leftOut), noted = true;
            return void(++perf.leftOutDraws);
        }
        bool prepared;
        {
            const Step step(perf, 2);
            prepared = pipeline.prepare(gpu, *state_, frame, kind, format);
        }
        if (!prepared) stop(pipeline.unsupported);
        if (scissorEmpty || !count) return;
        if (!inFrame) begin();
        const std::size_t remade = std::size_t(std::count_if(state_->passes.begin(), state_->passes.end(), [](const GxDrawState::Pass& p) { return !p.sameUnits; }));
        roomFor(GxVertexPipeline::maxVertices(kind, count) * (1 + remade), DrawCommandWords * u32(1 + state_->passes.size()));
        if (state_->color1InTexture) choosePaletteTexel(gpu);
        const Step step(perf, 6);
        const auto first = static_cast<std::uint16_t>(used);
        const std::size_t written = decoded ? pipeline.runDecoded(decoded, count, vertices + used, first, indices + indexUsed)
                                            : pipeline.run(data, count, vertices + used, first, indices + indexUsed);
        if (pipeline.takeAtlasOutside()) {
            refuseAtlas(boundAtlas);
            stateValid = false;
            return draw(gpu, kind, format, count, data, decoded);
        }
        indexUsed += written;
        used += pipeline.verticesWritten;
        if (remade) stagePasses(gpu, kind, format, count, data, decoded, vertices + first);
        fresh[0] = fresh[1] = fresh[2] = false;  // a readback must flush what's pending first
    }

    // The draw state for what comes next: kept while the BP registers a draw reads (C0 and C1 alone, when they ride)
    // stay. The CPU path's state may read C1 from the palette or sample an atlas; a mesh's never does.
    void ensureState(const GxCommandProcessor& gpu, bool cpuPath) {
        if (stateValid && !cpuPath && (state_->color1InTexture || boundAtlas >= 0)) return buildState(gpu, false);
        if (!stateValid || gpu.bpChanges != stateBp || !(frame == stateFrame)) {
            std::uint8_t color[4];
            const bool colorsOnly = gpu.bpChangesBesideColors == stateBesideColors && (state_->color1InTexture || gpu.color1Changes == stateColor1);
            if (stateValid && frame == stateFrame && state_->color0InVertex && (colorsOnly || onlyColor0Changed(gpu)) && gxColor0Bytes(gpu, color) &&
                (!state_->color1InTexture || gxColor1Bytes(gpu, color)))
                stateBp = gpu.bpChanges, stateBesideColors = gpu.bpChangesBesideColors, stateColor1 = gpu.color1Changes, ++perf.color0Rides;
            else
                buildState(gpu, cpuPath);
        } else {
            ++perf.sameState;
        }
    }

    // C1 for states that read it from the palette unit: 8x8 RGBA8 textures of 64 colors each, taken in order through a
    // command list (startBuffers starts over), so the texels a recorded draw samples never change under it.
    static constexpr unsigned PaletteTextures = 64, PaletteSlots = 64;
    static_assert(PaletteSlots == sizeof sPaletteTexels.slot / sizeof *sPaletteTexels.slot);
    C3D_Tex palettes[PaletteTextures];
    unsigned paletteUsed = 0, paletteFlushed = 0;
    int batchPalette = -1;  // the palette the pending batch samples
    u32 paletteColor = 0;  // the last texel's
    void choosePaletteTexel(const GxCommandProcessor& gpu) {
        std::uint8_t c[4];
        gxColor1Bytes(gpu, c);  // in range, or the state would not read it from here
        const u32 rgba = u32(c[0]) << 24 | u32(c[1]) << 16 | u32(c[2]) << 8 | c[3];
        if (paletteUsed && rgba == paletteColor && int((paletteUsed - 1) / PaletteSlots) == batchPalette) return;
        if (paletteUsed == PaletteTextures * PaletteSlots) pause(), resume();
        const unsigned slot = paletteUsed++, texture = slot / PaletteSlots;
        const PaletteTexel& texel = sPaletteTexels.slot[slot % PaletteSlots];
        if (int(texture) != batchPalette) flushBatch(), batchPalette = int(texture), ++stateVersion;
        paletteColor = rgba;
        std::memcpy(static_cast<std::uint8_t*>(palettes[texture].data) + 4 * texel.offset, &rgba, 4);
        pipeline.setPaletteTexel(texel.s, texel.t);
    }

    // readGxDrawState through a cache keyed by the registers it reads for the stages and maps in use (gxDrawStateKey):
    // J3D's materials come back every frame, so most builds are a lookup. The key is hashed by 32-bit multiplies, and the
    // draw uses the cached state where it lies.
    static constexpr unsigned MaxStateWords = MaxDrawStateKey + 1;
    struct CachedState {
        u32 words[MaxStateWords];
        std::size_t count = 0;
        u32 hash = 0;  // of the words, the entry's key in `states`
        GxDrawState state;
        u32 colors[4];  // C0 and C1 as read, for a marked key whose state kept them
        PicaPixel pixel;  // preparePixel's, of the state's material and program
        bool pixelReady = false;
        // The scissor on the target, made for `scissorFrame` (never a real frame until it is made).
        GxFrame scissorFrame{0, 0, false};
        bool scissorEmpty = false, scissorWhole = true;
        int scissor[4] = {};
        std::vector<PicaPixel> passPixels;  // its further passes', their blends made to add
        // CPU path, one map on one unit: a hash of the key without that map's size and address (shared by states that
        // differ only in which texture they sample), the texture, and its place in an atlas.
        bool oneMap = false;
        std::uint8_t mapUnit = 0;
        u32 base = 0;
        Key texture{};
        int atlas = -1;
        float place[4] = {};
        u32 atlasGeneration = 0;
    };
    std::unordered_map<u32, CachedState> states;
    static constexpr u32 StateSlots = 1024;
    CachedState* stateIndex[StateSlots] = {};
    u32 stateWords[MaxStateWords];
    CachedState* stateEntry = nullptr;  // the entry the last read came from or went into

    void countApproximation(const char* reason) {
        for (auto& [why, count] : perf.approximated)
            if (!why || !std::strcmp(why, reason)) {
                why = reason, ++count;
                return;
            }
    }

    // The key for the state GX holds now (stateWords), and its cached entry if there is one.
    u32 keyHash = 0;
    std::size_t keyCount = 0, keyMapsAt = 0;
    unsigned keyMarkers = 0;
    CachedState* findState(const GxCommandProcessor& gpu, bool palette) {
        keyMarkers = 0;
        std::size_t count = gxDrawStateKey(gpu, stateWords, palette, &keyMarkers, &keyMapsAt);
        stateWords[count++] = palette;
        u32 hash = 0x811C9DC5u;
        std::size_t i = 0;
        for (; i + 4 <= count; i += 4) {
            hash = (hash ^ stateWords[i]) * 0x01000193u, hash = (hash ^ stateWords[i + 1]) * 0x01000193u;
            hash = (hash ^ stateWords[i + 2]) * 0x01000193u, hash = (hash ^ stateWords[i + 3]) * 0x01000193u;
        }
        for (; i < count; ++i) hash = (hash ^ stateWords[i]) * 0x01000193u;
        keyHash = hash, keyCount = count;
        // A marked color holds for every value only if the state rides it; one whose program kept the value (exact only
        // that way) holds for that value.
        const auto holds = [&](const CachedState& c) {
            return (!(keyMarkers & 1) || c.state.color0InVertex || (c.colors[0] == gpu.tevColor[2] && c.colors[1] == gpu.tevColor[3])) &&
                   (!(keyMarkers & 2) || c.state.color1InTexture || (c.colors[2] == gpu.tevColor[4] && c.colors[3] == gpu.tevColor[5]));
        };
        // The entry recently found by this slot of the hash, before the map's own lookup (a division).
        CachedState* entry = stateIndex[hash % StateSlots];
        if (!entry || entry->hash != hash) {
            const auto found = states.find(hash);
            entry = found == states.end() ? nullptr : &found->second;
            if (entry) stateIndex[hash % StateSlots] = entry;
        }
        if (!entry || entry->count != count || !sameWords(entry->words, stateWords, count) || !holds(*entry)) return nullptr;
        ++perf.stateHits;
        return entry;
    }
    // The state for findState's key, read from GX (a key that collides takes its entry over).
    CachedState& makeState(const GxCommandProcessor& gpu, bool palette) {
        ++perf.stateMisses;
        if (states.size() >= 512) states.clear(), std::fill(std::begin(stateIndex), std::end(stateIndex), nullptr), ++perf.stateClears;
        CachedState& entry = states[keyHash];
        entry.hash = keyHash, stateIndex[keyHash % StateSlots] = &entry;
        std::memcpy(entry.words, stateWords, keyCount * sizeof(u32)), entry.count = keyCount;
        readGxDrawState(gpu, entry.state, palette);
        std::memcpy(entry.colors, gpu.tevColor + 2, sizeof entry.colors);
        if (!entry.state.program.exact) countApproximation(entry.state.program.approximation);
        entry.pixelReady = false, entry.scissorFrame = GxFrame{0, 0, false};
        describeForAtlas(entry, palette);
        return entry;
    }

    // Particles that pick one of a few textures each (Mario's running dust) ended the batch at every particle; textures
    // taking turns under one otherwise equal state share an atlas instead.
    struct Atlas {
        u32 base = 0;
        std::vector<Key> members;
        std::vector<std::array<float, 4>> place;
        C3D_Tex texture{};
        bool built = false, refused = false;
    };
    // Many effects share a base: only textures taking turns together lately are worth an atlas.
    struct Turns {
        std::vector<Key> textures;
        u32 count = 0, since = 0, builds = 0, failures = 0;
        bool refused = false;
    };
    static constexpr unsigned AtlasMembers = 8, AtlasTurns = 24, AtlasSide = 256, AtlasWindow = 8, AtlasBuilds = 6;
    u32 framesShown = 0;
    std::deque<Atlas> atlases;  // citro3d keeps a bound texture's address: atlases never move
    std::unordered_map<u32, unsigned> atlasByBase;
    std::unordered_map<u32, Turns> turnsByBase;
    std::unordered_set<u32> atlasAddresses;  // members' texels, whose change drops every atlas
    u32 atlasGeneration = 1;
    int boundAtlas = -1;  // the atlas the pending batch's coordinates were made for

    void describeForAtlas(CachedState& e, bool palette) {
        e.oneMap = false, e.atlas = -1, e.atlasGeneration = 0;
        if (!palette || e.state.unsupported || !e.state.passes.empty()) return;
        int unit = -1, map = -1;
        for (unsigned u = 0; u < e.state.program.unitCount; ++u) {
            const int t = e.state.program.units[u].texture;
            if (t < 0) continue;  // no map, or the palette's
            if (unit >= 0) return;
            unit = int(u), map = t;
        }
        if (unit < 0 || map < 0 || map >= 8) return;
        const GxTextureMap& m = e.state.textures[map];
        if (m.wrapS > 2 || m.wrapT > 2 || gpuCopies.count(m.address)) return;
        // Without the maps' sizes and addresses, but with their formats: only textures of one format share an atlas.
        u32 hash = 0x811C9DC5u;
        for (std::size_t i = 0; i < e.count; ++i) {
            const std::size_t word = i >= keyMapsAt && i + 1 < e.count ? (i - keyMapsAt) % 3 : 0;
            hash = (hash ^ (word == 0 ? e.words[i] : word == 1 ? e.words[i] & 0x00F00000u : 0)) * 0x01000193u;
        }
        e.oneMap = true, e.mapUnit = static_cast<std::uint8_t>(unit), e.base = hash;
        e.texture = Key{m.address, m.width, m.height, m.format, m.wrapS, m.wrapT, m.minFilter, m.magFilter, m.maxLod, m.lodBias};
    }

    void placeInAtlas(CachedState& e) {
        if (e.atlasGeneration == atlasGeneration) return;
        e.atlasGeneration = atlasGeneration, e.atlas = -1;
        if (!e.oneMap) return;
        const auto found = atlasByBase.find(e.base);
        if (found == atlasByBase.end()) return;
        const Atlas& a = atlases[found->second];
        if (!a.built || a.refused) return;
        for (std::size_t k = 0; k < a.members.size(); ++k)
            if (a.members[k] == e.texture) {
                e.atlas = int(found->second);
                std::memcpy(e.place, a.place[k].data(), sizeof e.place);
                return;
            }
    }

    // True when it built an atlas: the pending batch was drawn first if an old one went.
    bool noteTurn(const CachedState& previous, const CachedState& next) {
        if (!previous.oneMap || !next.oneMap || previous.base != next.base || previous.texture == next.texture) return false;
        Turns& turns = turnsByBase[previous.base];
        if (turns.refused) return false;
        ++perf.atlasTurns;
        if (framesShown - turns.since > AtlasWindow) turns.textures.clear(), turns.count = 0;
        if (turns.textures.empty()) turns.since = framesShown;
        for (const Key& k : {previous.texture, next.texture})
            if (std::find(turns.textures.begin(), turns.textures.end(), k) == turns.textures.end()) turns.textures.push_back(k);
        if (turns.textures.size() > AtlasMembers) turns.textures = {previous.texture, next.texture}, turns.count = 0, turns.since = framesShown;
        if (++turns.count < AtlasTurns) return false;
        std::vector<Key> keys = std::move(turns.textures);
        turns.textures.clear(), turns.count = 0;
        const auto found = atlasByBase.find(previous.base);
        if (found != atlasByBase.end()) {
            const Atlas& atlas = atlases[found->second];
            std::vector<Key> joined = atlas.members;
            for (const Key& k : keys)
                if (std::find(joined.begin(), joined.end(), k) == joined.end()) joined.push_back(k);
            if (atlas.built && joined.size() == atlas.members.size()) return false;
            if (joined.size() <= AtlasMembers) keys = std::move(joined);
        }
        // Effects whose textures can't share one, or that keep changing it, are left to draw apart.
        if (++turns.builds > AtlasBuilds || (!buildAtlas(previous.base, keys) && ++turns.failures >= 3)) turns.refused = true;
        return true;
    }

    bool buildAtlas(u32 base, const std::vector<Key>& keys) {
        // Members of one GX format and power-of-two sizes, in the PICA format that holds each of them: the one they share,
        // else gray with alpha for gray ones, else RGBA8 (a member's texels then a little finer than its own texture's).
        std::vector<ModelTexture> members(keys.size());
        GPU_TEXCOLOR format = GPU_RGBA8;
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const auto& [address, width, height, fmt, wrapS, wrapT, minFilter, magFilter, maxLod, lodBias] = keys[k];
            if (fmt != std::get<3>(keys[0]) || fmt == 8 || fmt == 9 || fmt == 10 || fmt == 14 || (width & (width - 1)) || (height & (height - 1)))
                return false;
            ModelTexture& m = members[k];
            m.format = static_cast<GxFormat>(fmt);
            std::string error;
            const std::size_t size = gxImageSize(m.format, width, height);
            if (!size || !decodeGxImage({static_cast<const std::uint8_t*>(OSPhysicalToCached(address)), size}, m.format, width, height, {},
                                        GxPalette::IA8, m.image, error))
                return false;
            const GPU_TEXCOLOR chosen = tileFormat(m);
            const auto gray = [](GPU_TEXCOLOR f) { return f == GPU_L8 || f == GPU_LA4 || f == GPU_LA8; };
            if (chosen == GPU_ETC1 || chosen == GPU_ETC1A4) return false;
            format = !k || chosen == format ? chosen : gray(chosen) && gray(format) ? GPU_LA8 : GPU_RGBA8;
        }
        // A border of the texels each member's wrap mode samples past its edges keeps filtering inside it.
        std::vector<std::size_t> order(keys.size());
        for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return members[a].image.height > members[b].image.height; });
        unsigned atlasWidth = 8, atlasHeight = 8;
        std::vector<std::array<unsigned, 2>> at(keys.size());
        for (;; atlasWidth *= 2) {
            if (atlasWidth > AtlasSide) return false;
            unsigned x = 0, y = 0, shelf = 0;
            bool fits = true;
            for (std::size_t k : order) {
                const unsigned w = members[k].image.width + 2, h = members[k].image.height + 2;
                if (w > atlasWidth) { fits = false; break; }
                if (x + w > atlasWidth) x = 0, y += shelf, shelf = 0;
                at[k] = {x + 1, y + 1}, x += w, shelf = std::max(shelf, h);
            }
            atlasHeight = 8;
            while (atlasHeight < y + shelf) atlasHeight *= 2;
            if (fits && atlasHeight <= AtlasSide) break;
        }
        ModelTexture packed;
        packed.format = members[0].format;
        packed.image.width = atlasWidth, packed.image.height = atlasHeight, packed.image.grayscale = true;
        packed.image.pixels.assign(std::size_t(atlasWidth) * atlasHeight, 0);
        std::vector<std::array<float, 4>> place(keys.size());
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const Image& image = members[k].image;
            packed.image.grayscale &= image.grayscale;
            const int w = int(image.width), h = int(image.height);
            const auto beyond = [](int i, int size, int wrap) { return wrap == 1 ? (i + size) % size : std::clamp(i, 0, size - 1); };
            for (int y = -1; y <= h; ++y)
                for (int x = -1; x <= w; ++x)
                    packed.image.pixels[std::size_t(int(at[k][1]) + y) * atlasWidth + std::size_t(int(at[k][0]) + x)] =
                        image.pixels[std::size_t(beyond(y, h, std::get<5>(keys[k]))) * image.width + std::size_t(beyond(x, w, std::get<4>(keys[k])))];
            place[k] = {float(w) / float(atlasWidth), float(h) / float(atlasHeight), float(at[k][0]) / float(atlasWidth), float(at[k][1]) / float(atlasHeight)};
        }
        packed.minFilter = std::get<6>(keys[0]) & 4 ? 1 : 0, packed.magFilter = std::get<7>(keys[0]);
        const TiledTexture tiled = tile(packed, format);
        auto found = atlasByBase.find(base);
        if (found == atlasByBase.end()) found = atlasByBase.emplace(base, unsigned(atlases.size())).first, atlases.emplace_back();
        Atlas& atlas = atlases[found->second];
        if (atlas.built) {
            flushBatch();  // the pending batch may sample the old one
            if (inFrame) pause(), resume();
            C3D_TexDelete(&atlas.texture);
            atlas.built = false;
        }
        if (!upload(atlas.texture, tiled)) {
            atlas.refused = true, ++atlasGeneration;  // its old texture is already gone
            return false;
        }
        atlas.base = base, atlas.members = keys, atlas.place = std::move(place), atlas.built = true, atlas.refused = false;
        for (const Key& k : keys) atlasAddresses.insert(std::get<0>(k));
        ++atlasGeneration, ++perf.atlasesBuilt;
        return true;
    }

    void refuseAtlas(int index) {
        if (index < 0) return;
        atlases[std::size_t(index)].refused = true;
        turnsByBase[atlases[std::size_t(index)].base].refused = true;
        ++atlasGeneration;
    }

    // A member's texels changed, or every texture went: the atlases go too (built again as their textures take turns).
    void dropAtlases() {
        if (atlases.empty() && turnsByBase.empty()) return;
        flushBatch();
        if (inFrame && !atlases.empty()) pause(), resume();
        for (Atlas& a : atlases)
            if (a.built) C3D_TexDelete(&a.texture);
        atlases.clear(), atlasByBase.clear(), turnsByBase.clear(), atlasAddresses.clear();
        ++atlasGeneration, boundAtlas = -1, stateValid = false;
        pipeline.clearAtlas();
    }

    // dev.gx_timing: the clock around one of the per-call steps.
    struct Step {
        Step(Perf& perf, int which) : perf_(perf), which_(which), zone_(zones[which]), start_(timing() ? svcGetSystemTick() : 0) {}
        ~Step() {
            if (start_) perf_.stepTicks[which_] += svcGetSystemTick() - start_, ++perf_.steps[which_];
        }
        static bool timing() { return sStepTiming; }
        static constexpr GxZone zones[13] = {ZoneState,   ZoneApply,  ZonePrepare,    ZoneState,    ZoneState, ZoneState,    ZoneCpuVertices,
                                             ZoneFlush,   ZoneMeshLookup, ZoneUniforms, ZoneMeshDraw, ZoneTexture, ZoneMeshBuild};
        Perf& perf_;
        int which_;
        GxZoneScope zone_;
        u64 start_;
    };

    // dev.gx_timing: a step's clock where a scope doesn't fit.
    u64 stepStart() const { return Step::timing() ? svcGetSystemTick() : 0; }
    void stepEnd(int which, u64 start) {
        if (start) perf.stepTicks[which] += svcGetSystemTick() - start, ++perf.steps[which];
    }

    // Read back only for a riding C0 (onlyColor0Changed) and by the timing's tally.
    void recordBuild(const GxCommandProcessor& gpu) {
        if (!state_->color0InVertex && !Step::timing()) return;
        std::memcpy(bpAtBuild, gpu.bp, sizeof bpAtBuild);
        std::memcpy(colorAtBuild, gpu.tevColor, sizeof colorAtBuild);
        std::memcpy(konstAtBuild, gpu.tevKonst, sizeof konstAtBuild);
    }

    // A joining state is drawn with the batch's combiner, so its colors must be the batch's unless both read them per
    // vertex (C0) or from the palette (C1).
    static bool sameColors(const CachedState& a, const CachedState& b) {
        return ((a.state.color0InVertex && b.state.color0InVertex) || !std::memcmp(a.colors, b.colors, 2 * sizeof(u32))) &&
               ((a.state.color1InTexture && b.state.color1InTexture) || !std::memcmp(a.colors + 2, b.colors + 2, 2 * sizeof(u32)));
    }

    void buildState(const GxCommandProcessor& gpu, bool cpuPath) {
        CachedState* previous = stateValid && cpuPath ? stateEntry : nullptr;
        CachedState* next;
        {
            const Step read(perf, 3);
            next = findState(gpu, cpuPath);
        }
        if (previous && next && previous != next) {
            if (noteTurn(*previous, *next)) previous = nullptr;  // an atlas made or remade: the batch is drawn, start afresh
            if (previous) placeInAtlas(*next);
            if (previous && boundAtlas >= 0 && next->atlas == boundAtlas && frame == stateFrame && sameColors(*previous, *next)) {
                // The same state but its texture, which sits in the batch's atlas: the particle joins the batch.
                stateEntry = next, state_ = &next->state;
                pipeline.stateChanged();
                pipeline.setAtlas(next->mapUnit, next->place);
                recordBuild(gpu);
                stateBp = gpu.bpChanges, stateBesideColors = gpu.bpChangesBesideColors, stateColor1 = gpu.color1Changes;
                ++perf.atlasJoins;
                return;
            }
        }
        flushBatch();
        const Step step(perf, 0);
        if (!next) {
            const Step read(perf, 3);
            next = &makeState(gpu, cpuPath);
        }
        stateEntry = next, state_ = &next->state;
        boundAtlas = -1;
        if (cpuPath) placeInAtlas(*next), boundAtlas = next->atlas;
        if (boundAtlas >= 0) pipeline.setAtlas(next->mapUnit, next->place);
        else pipeline.clearAtlas();
        pipeline.stateChanged();
        if (Step::timing()) {
            const Step stats(perf, 4);
            for (u32 a = 0; a < 0x100; ++a)
                if (gpu.bp[a] != bpAtBuild[a] && GxCommandProcessor::drawsRead(a)) ++perf.changed[a];
        }
        recordBuild(gpu);
        stateValid = true, stateBp = gpu.bpChanges, stateBesideColors = gpu.bpChangesBesideColors, stateColor1 = gpu.color1Changes;
        stateFrame = frame, ++stateVersion;
        if (state_->unsupported) stop(state_->unsupported);
        if (state_->material.blend.type == 2 && state_->material.blend.logic != 3) stop("a logic op other than copy");
        if (state_->depthTextureOp) depthFromTexture(*state_);
        if (state_->indirectDropped && !indirectNoted) {
            std::fprintf(stderr, "The Wii GPU's drawing: indirect texture offsets are left out (the PICA has no dependent reads)\n");
            indirectNoted = true;
        }
        for (int i = 0; i < 6; ++i) fogWords[i] = gpu.bp[GxFogRegisters[i]];
        if (!TEV_FOG_PARAM_3_GET_FSEL(fogWords[4])) std::fill(std::begin(fogWords), std::end(fogWords), 0u);  // off, whatever the rest
        if ((fogWords[0] >> 10 & 1) && !fogRangeNoted) {
            std::fprintf(stderr, "The Wii GPU's drawing: fog's range adjustment is left out\n");
            fogRangeNoted = true;
        }
        // GX's scissor in screen pixels, then on the target turned a quarter as the LCD is (citro3d's convention), kept per entry and frame.
        if (!(stateEntry->scissorFrame == frame)) {
            const int x0 = std::clamp(static_cast<int>(std::lround(state_->scissor[0] * 400.0f / frame.width)), 0, 400);
            const int x1 = std::clamp(static_cast<int>(std::lround(state_->scissor[2] * 400.0f / frame.width)), 0, 400);
            const int y0 = std::clamp(static_cast<int>(std::lround(state_->scissor[1] * 240.0f / frame.height)), 0, 240);
            const int y1 = std::clamp(static_cast<int>(std::lround(state_->scissor[3] * 240.0f / frame.height)), 0, 240);
            stateEntry->scissorFrame = frame;
            stateEntry->scissorEmpty = x1 <= x0 || y1 <= y0;
            stateEntry->scissorWhole = x0 == 0 && y0 == 0 && x1 == 400 && y1 == 240;
            stateEntry->scissor[0] = 240 - y1, stateEntry->scissor[1] = 400 - x1, stateEntry->scissor[2] = 240 - y0, stateEntry->scissor[3] = 400 - x0;
        }
        scissorEmpty = stateEntry->scissorEmpty, scissorWhole = stateEntry->scissorWhole;
        std::memcpy(scissor, stateEntry->scissor, sizeof scissor);
        gpuXf[0] = gpu.xf[0x1009], gpuXf[1] = gpu.xf[0x100E], gpuXf[2] = gpu.xf[0x1010], gpuXf[3] = gpu.xf[0x100C], gpuXf[4] = gpu.xf[0x100A];
        {
            const Step prepare(perf, 5);
            if (!stateEntry->pixelReady) stateEntry->pixel = preparePixel(state_->material, state_->program), stateEntry->pixelReady = true;
            pixelNow = &stateEntry->pixel;
        }
        if (state_->dstAlphaEnable && state_->alphaUpdate) keepDstAlpha();
        writeMask = (state_->colorUpdate ? GPU_WRITE_COLOR & ~GPU_WRITE_ALPHA : 0) | (state_->alphaUpdate ? GPU_WRITE_ALPHA : 0) |
                    (pixelNow->depthWrite ? GPU_WRITE_DEPTH : 0);
    }

    // GXSetDstAlpha: the EFB takes a constant alpha while the TEV's blends and is tested. The PICA blends and tests with
    // the alpha it writes, so that holds exactly only when neither reads the source alpha; then a last stage replaces it.
    void keepDstAlpha() {
        const auto readsSource = [](GPU_BLENDFACTOR f) { return f == GPU_SRC_ALPHA || f == GPU_ONE_MINUS_SRC_ALPHA; };
        if (readsSource(pixelNow->source) || readsSource(pixelNow->destination) || pixelNow->alphaTest) {
            if (!dstAlphaNoted) std::fprintf(stderr, "The Wii GPU's drawing: destination alpha with a blend or test that reads the source alpha keeps the source alpha\n");
            dstAlphaNoted = true;
            return;
        }
        ownPixel = *pixelNow, pixelNow = &ownPixel;
        PicaPixel& pixel = ownPixel;
        const unsigned last = std::min(state_->program.stageCount, CombinerProgram::MaxStages - 1);
        C3D_TexEnv& env = pixel.env[last];
        if (last == state_->program.stageCount) {
            C3D_TexEnvInit(&env);
            C3D_TexEnvSrc(&env, C3D_RGB, GPU_PREVIOUS);
        } else if ((env.srcRgb & 0xF) == GPU_CONSTANT || (env.srcRgb >> 4 & 0xF) == GPU_CONSTANT || (env.srcRgb >> 8 & 0xF) == GPU_CONSTANT) {
            if (!dstAlphaNoted) std::fprintf(stderr, "The Wii GPU's drawing: destination alpha on a sixth stage that reads its constant shares the constant\n");
            dstAlphaNoted = true;
        }
        C3D_TexEnvSrc(&env, C3D_Alpha, GPU_CONSTANT);
        C3D_TexEnvOpAlpha(&env, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(&env, C3D_Alpha, GPU_REPLACE);
        env.color = (env.color & 0x00FFFFFF) | u32(state_->dstAlpha) << 24;
        pixel.alphaEquation = GPU_BLEND_ADD, pixel.alphaSource = GPU_ONE, pixel.alphaDestination = GPU_ZERO;
    }

    // One draw for the dump: its size, where it lands on the screen (EFB pixels), and the state that draws it.
    bool dumpBatch() {
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (std::size_t i = batchStart; i < indexUsed; ++i) {
            const float* p = vertices[indices[i]].position;
            if (p[3] <= 0) continue;
            // Undo the quarter turn: x = -y / w, y = x / w in [-1, 1], then EFB pixels with y down.
            const float ex = (-p[1] / p[3] + 1) * frame.width / 2, ey = (1 - p[0] / p[3]) * frame.height / 2;
            lo[0] = std::min(lo[0], ex), lo[1] = std::min(lo[1], ey), hi[0] = std::max(hi[0], ex), hi[1] = std::max(hi[1], ey);
        }
        const ModelMaterial& m = state_->material;
        std::string stages;
        for (int s = 0; s < m.tevStageCount; ++s) {
            const TevStage& t = m.stages[s];
            char text[96];
            std::snprintf(text, sizeof text, " [%x%x%x%x op%u %x%x%x%x op%u map%d ch%u]", t.colorIn[0], t.colorIn[1], t.colorIn[2], t.colorIn[3], t.colorOp,
                          t.alphaIn[0], t.alphaIn[1], t.alphaIn[2], t.alphaIn[3], t.alphaOp, t.texMap == 0xFF ? -1 : t.texMap, t.channel);
            stages += text;
        }
        std::string maps;
        for (unsigned u = 0; u < state_->program.unitCount; ++u) {
            if (state_->program.units[u].texture == CombinerProgram::Palette) {
                maps += " C1-palette";
                continue;
            }
            const GxTextureMap& t = state_->textures[state_->program.units[u].texture];
            char text[64];
            std::snprintf(text, sizeof text, " %08lx:%ux%u:f%u", static_cast<unsigned long>(t.address), t.width, t.height, t.format);
            maps += text;
        }
        // The colors the combiner reads: konst and C0, and what the vertices' color (the rasterized channel) came out as.
        unsigned colorLo[4] = {255, 255, 255, 255}, colorHi[4] = {}, colorSum[4] = {}, counted = 0;
        for (std::size_t i = batchStart; i < indexUsed && counted < 4096; ++i, ++counted)
            for (int c = 0; c < 4; ++c) {
                const unsigned v = vertices[indices[i]].color[c];
                colorLo[c] = std::min(colorLo[c], v), colorHi[c] = std::max(colorHi[c], v), colorSum[c] += v;
            }
        const auto byte = [](float v) { return int(std::lround(v * 255)); };
        std::string konst;
        for (int k = 0; k < 4; ++k) {
            char text[48];
            std::snprintf(text, sizeof text, " K%d=%d,%d,%d,%d", k, byte(m.konstColors[k][0]), byte(m.konstColors[k][1]), byte(m.konstColors[k][2]),
                          byte(m.konstColors[k][3]));
            konst += text;
        }
        std::fprintf(stderr, "gx dump:   colors: vertex lo %u,%u,%u,%u avg %u,%u,%u,%u hi %u,%u,%u,%u; C0=%d,%d,%d,%d%s; ksel %x/%x; xf chans %lu ctrl "
                     "%08lx/%08lx mat %08lx amb %08lx\n", colorLo[0], colorLo[1], colorLo[2], colorLo[3], counted ? colorSum[0] / counted : 0,
                     counted ? colorSum[1] / counted : 0, counted ? colorSum[2] / counted : 0, counted ? colorSum[3] / counted : 0, colorHi[0],
                     colorHi[1], colorHi[2], colorHi[3], byte(m.tevColors[0][0]), byte(m.tevColors[0][1]), byte(m.tevColors[0][2]),
                     byte(m.tevColors[0][3]), konst.c_str(), m.stages[0].konstColor, m.stages[0].konstAlpha, static_cast<unsigned long>(gpuXf[0]),
                     static_cast<unsigned long>(gpuXf[1]), static_cast<unsigned long>(gpuXf[2]), static_cast<unsigned long>(gpuXf[3]),
                     static_cast<unsigned long>(gpuXf[4]));
        std::fprintf(stderr, "gx dump: draw %u: %u tris, box (%.0f,%.0f)-(%.0f,%.0f); blend %u %u/%u, color %d alpha %d, dst alpha %d:%u, z %d/%u/%d, "
                     "alpha test %u%s%u, C0 %s; tev%s; maps%s%s\n", dumpIndex++, unsigned((indexUsed - batchStart) / 3), lo[0], lo[1], hi[0], hi[1],
                     m.blend.type, m.blend.source, m.blend.destination, state_->colorUpdate, state_->alphaUpdate, state_->dstAlphaEnable, state_->dstAlpha,
                     m.depth.enable, m.depth.function, m.depth.update, m.alphaTest.compare0, m.alphaTest.op ? "|" : "&", m.alphaTest.compare1,
                     state_->color0InVertex ? "rides" : "const", stages.c_str(), maps.c_str(), state_->program.exact ? "" : " (approximated)");
        return lo[0] <= 1 && lo[1] <= 1 && hi[0] >= frame.width - 1 && hi[1] >= frame.height - 1;
    }

    // A mesh draw for the dump: its size and the state that draws it.
    void dumpMesh(const CachedMesh& mesh, const GxMeshUniforms& u) {
        const ModelMaterial& m = state_->material;
        std::string stages;
        for (int s = 0; s < m.tevStageCount; ++s) {
            const TevStage& t = m.stages[s];
            char text[96];
            std::snprintf(text, sizeof text, " [%x%x%x%x op%u %x%x%x%x op%u map%d ch%u]", t.colorIn[0], t.colorIn[1], t.colorIn[2], t.colorIn[3], t.colorOp,
                          t.alphaIn[0], t.alphaIn[1], t.alphaIn[2], t.alphaIn[3], t.alphaOp, t.texMap == 0xFF ? -1 : t.texMap, t.channel);
            stages += text;
        }
        std::fprintf(stderr, "gx dump: draw %u (mesh): %u tris in %zu groups, lit %d (%u entries, rgb %d alpha %d), cull %u; blend %u %u/%u, color %d "
                     "alpha %d, dst alpha %d:%u, z %d/%u/%d, alpha test %u%s%u; tev%s\n", dumpIndex++, mesh.indexCount / 3, mesh.groups.size(), u.lit,
                     u.lights, u.rgbLit, u.alphaLit, u.reject, m.blend.type, m.blend.source, m.blend.destination, state_->colorUpdate, state_->alphaUpdate,
                     state_->dstAlphaEnable, state_->dstAlpha, m.depth.enable, m.depth.function, m.depth.update, m.alphaTest.compare0,
                     m.alphaTest.op ? "|" : "&", m.alphaTest.compare1, stages.c_str());
        if (!u.lit) return;
        // A lit mesh's lighting as the shader's model works it out for a few of its first group's vertices, and its light
        // entries: which part of the input makes a draw come out dark.
        for (unsigned e = 0; e < u.lights; ++e) {
            const float(&l)[6][4] = u.light[e];
            std::fprintf(stderr, "gx dump:   light %u: color %.2f %.2f %.2f %.2f, position %.3g %.3g %.3g, direction %.3f %.3f %.3f, spot %.0f diffuse %.0f\n",
                         e, l[0][0], l[0][1], l[0][2], l[0][3], l[1][0] * l[4][1], l[1][1] * l[4][1], l[1][2] * l[4][1], l[2][0],
                         l[2][1], l[2][2], l[5][0], l[5][1]);
        }
        for (u32 i = 0; i < std::min<u32>(mesh.vertexCount, 3); ++i) {
            const GxMeshVertex v = gxUnpackMeshVertex(mesh.layout, static_cast<const std::uint8_t*>(mesh.vertices) + i * mesh.layout.stride);
            float clip[4], color[4], tex[CombinerProgram::MaxUnits][2];
            gxMeshModel(u, v, clip, color, tex);
            const unsigned slot = (unsigned(v.slot) - 4) / u.slotRows;
            const float* n = u.normal[slot < GxMesh::MaxLitSlots ? slot : 0];
            std::fprintf(stderr, "gx dump:   vertex %lu (slot %u): normal %.2f %.2f %.2f, normal rows %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f, "
                         "ambient %.2f %.2f %.2f, lit color %.0f %.0f %.0f %.0f\n", static_cast<unsigned long>(i), slot, v.normal[0], v.normal[1], v.normal[2],
                         n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], u.ambient[0], u.ambient[1], u.ambient[2], color[0] * 255, color[1] * 255,
                         color[2] * 255, color[3] * 255);
        }
    }

    // Meshes may always take MeshBudget, and more while MeshReserve of linear memory stays free for textures and movies.
    static constexpr std::size_t MeshBudget = 12u << 20, MeshReserve = 8u << 20;

    void dropMesh(std::unordered_map<std::uint64_t, CachedMesh>::iterator it) {
        meshBytes -= it->second.vertexCount * it->second.layout.stride + it->second.indexCount * sizeof(std::uint16_t);
        linearFree(it->second.block);
        if (CachedMesh*& slot = meshIndex[u32(it->first) % MeshSlots]; slot == &it->second) slot = nullptr;
        meshes.erase(it);
    }
    // Room for `bytes` more, from the meshes least recently drawn: none drawn in this frame or the last, which the GPU
    // may still read.
    void evictMeshesFor(std::size_t bytes, std::size_t& free) {
        std::vector<std::pair<u32, std::uint64_t>> stale;
        for (const auto& [hash, mesh] : meshes)
            if (mesh.lastFrame + 2 <= framesShown) stale.emplace_back(mesh.lastFrame, hash);
        std::sort(stale.begin(), stale.end());
        for (const auto& [frame, hash] : stale) {
            if (meshBytes + bytes <= MeshBudget || free >= bytes + MeshReserve) break;
            const std::size_t held = meshBytes;
            dropMesh(meshes.find(hash)), ++perf.meshesEvicted, free += held - meshBytes;
        }
    }
    bool makeRoom(std::size_t bytes) {
        std::size_t free = meshBytes + bytes > MeshBudget ? linearSpaceFree() : 0;
        const auto room = [&] { return meshBytes + bytes <= MeshBudget || free >= bytes + MeshReserve; };
        if (!room()) evictMeshesFor(bytes, free);
        return room();
    }
    // Linear memory for something that would otherwise stop: the meshes the GPU no longer reads go, then (outside a draw,
    // whose mesh or batch dropping every mesh would pull out from under it) every mesh, with another try after each.
    bool drawing = false;
    template <class Allocate> bool withRoom(Allocate allocate) {
        if (allocate()) return true;
        const std::size_t held = meshes.size();
        for (auto it = meshes.begin(); it != meshes.end();) {
            const auto next = std::next(it);
            if (it->second.lastFrame + 2 <= framesShown) dropMesh(it);
            it = next;
        }
        if (meshes.size() < held && (++perf.meshesDroppedForRoom, allocate())) return true;
        if (drawing || meshes.empty()) return false;
        dropAllMeshes(), ++perf.meshesDroppedForRoom;
        return allocate();
    }
    void dropAllMeshes() {
        if (inFrame) pause(), resume();  // the GPU may still read them
        while (!meshes.empty()) dropMesh(meshes.begin());
        clearDecoded();  // its page marks go too
        meshPages.clear();
        for (auto& bits : sMeshPageBits) bits.store(0, std::memory_order_relaxed);
        for (std::uintptr_t page : texturePages) sMeshPageBits[page >> 5].fetch_or(1u << (page & 31), std::memory_order_relaxed);
    }

    // Where a texture's texels lie in the game's memory, [lo, hi).
    static std::pair<std::uintptr_t, std::uintptr_t> textureRange(const Key& key) {
        const auto lo = reinterpret_cast<std::uintptr_t>(OSPhysicalToCached(std::get<0>(key)));
        return {lo, lo + gxLevelOffset(static_cast<GxFormat>(std::get<3>(key)), std::get<1>(key), std::get<2>(key), mipLevels(key))};
    }

    // The CPU wrote memory the GPU will read (the game's cache flushes, noted in sChanged): meshes made from it go.
    std::uintptr_t changedStart[1024], changedEnd[1024];
    void applyChangedMemory() {
        std::uintptr_t* start = changedStart;
        std::uintptr_t* end = changedEnd;
        if (!sChanged.pending.load(std::memory_order_acquire)) return;
        const GxZoneScope zone(ZoneChanged);
        LightLock_Lock(&sChanged.lock);
        const unsigned count = sChanged.count;
        const bool overflowed = sChanged.overflowed;
        std::memcpy(start, sChanged.start, count * sizeof *start), std::memcpy(end, sChanged.end, count * sizeof *end);
        sChanged.count = 0, sChanged.overflowed = false;
        sChanged.pending.store(false, std::memory_order_relaxed);
        LightLock_Unlock(&sChanged.lock);
        perf.flushesQueued += count;
        if (meshes.empty() && decodedLists.empty() && textures.empty()) return;
        if (overflowed) return ++perf.meshOverflows, dropAllMeshes(), dropAllTextures();
        bool paused = false;
        for (unsigned i = 0; i < count; ++i) {
            bool touched = false, textureTouched = false;
            for (std::uintptr_t page = start[i] >> 12; page <= (end[i] - 1) >> 12; ++page)
                touched |= meshPages.count(page) != 0, textureTouched |= texturePages.count(page) != 0;
            // A new scene's files read over an old one's textures: what they now hold is drawn fresh.
            for (auto it = textures.begin(); textureTouched && it != textures.end();) {
                const auto [lo, hi] = textureRange(it->first);
                if (lo >= end[i] || start[i] >= hi) { ++it; continue; }
                if (atlasAddresses.count(std::get<0>(it->first))) dropAtlases();
                retired.push_back(it->second), ++perf.texturesInvalidated;
                it = textures.erase(it);
                stateValid = applied = false;  // its textures may be gone
                forgetSent();
            }
            if (!touched) continue;
            for (auto it = meshes.begin(); it != meshes.end();) {
                auto next = std::next(it);
                const auto& r = it->second.range;
                if ((r[0][0] < end[i] && start[i] < r[0][1]) || (r[1][0] < end[i] && start[i] < r[1][1])) {
                    if (inFrame && !paused) pause(), resume(), paused = true;  // the GPU may still read it
                    dropMesh(it), ++perf.meshInvalidated;
                }
                it = next;
            }
            for (auto it = decodedLists.begin(); it != decodedLists.end();) {
                const auto& r = it->second.range;
                const bool overlaps = (r[0][0] < end[i] && start[i] < r[0][1]) || (r[1][0] < end[i] && start[i] < r[1][1]);
                it = overlaps ? eraseDecoded(it) : std::next(it);
            }
        }
    }

    // GX_CallDisplayList: a list of primitives under a state the GPU path covers is drawn from its mesh (made the first
    // time); anything else is read by the command processor as before.
    bool displayList(const GxCommandProcessor& gpu, std::uint32_t address, std::uint32_t size) {
        const GxZoneScope zone(ZoneListCheck);
        const unsigned why = displayListMesh(gpu, address, size);
        listRefusal = why;
        if (why == GxVertexPipeline::MeshSmall) return drawDecodedList(gpu, address, size);
        return !why;
    }
    unsigned listRefusal = 0;

    // Small display lists (a particle's quad), their primitives decoded once and drawn on the CPU path from then on:
    // read again when the list's bytes or the CP registers it was read under differ, or memory it came from changes
    // (the meshes' page marks).
    struct DecodedList {
        u32 address = 0, size = 0, cpChanges = 0;
        std::uint8_t bytes[32] = {};
        std::vector<std::pair<std::uint8_t, u32>> depends;
        struct Primitive {
            std::uint8_t kind, format;
            std::uint16_t first, count;
        };
        std::vector<Primitive> primitives;
        std::vector<GxVertex> vertices;
        std::uintptr_t range[2][2] = {};  // the list, the array elements
    };
    std::unordered_map<u32, DecodedList> decodedLists;
    // A direct-mapped index in front of the map, whose lookup divides.
    static constexpr u32 DecodedSlots = 256;
    DecodedList* decodedIndex[DecodedSlots] = {};
    static u32 decodedSlot(u32 address) { return address >> 5 & (DecodedSlots - 1); }
    std::unordered_map<u32, DecodedList>::iterator eraseDecoded(std::unordered_map<u32, DecodedList>::iterator it) {
        if (DecodedList*& slot = decodedIndex[decodedSlot(it->first)]; slot == &it->second) slot = nullptr;
        return decodedLists.erase(it);
    }
    void clearDecoded() { decodedLists.clear(), std::fill(std::begin(decodedIndex), std::end(decodedIndex), nullptr); }
    // A list as it was: by words for the usual one block (the library's memcmp goes a byte at a time).
    static bool sameBytes(const std::uint8_t* kept, const std::uint8_t* list, u32 size) {
        if (size != 32) return !std::memcmp(kept, list, size);
        u32 differ = 0;
        for (int i = 0; i < 32; i += 4) {
            u32 a, b;
            std::memcpy(&a, kept + i, 4), std::memcpy(&b, list + i, 4);
            differ |= a ^ b;
        }
        return !differ;
    }

    bool drawDecodedList(const GxCommandProcessor& gpu, std::uint32_t address, std::uint32_t size) {
        applyChangedMemory();
        const GxZoneScope zone(ZoneSmallList);
        const std::uint8_t* list = gpu.memoryFunction()(address);
        DecodedList*& slot = decodedIndex[decodedSlot(address)];
        if (!slot || slot->address != address) {
            const auto it = decodedLists.find(address);
            slot = it == decodedLists.end() ? nullptr : &it->second;
        }
        DecodedList* kept = slot;
        bool valid = kept && kept->size == size && sameBytes(kept->bytes, list, size);
        if (valid && kept->cpChanges != gpu.cpChanges) {
            for (const auto& [reg, value] : kept->depends) valid &= gpu.cp[reg] == value;
            if (valid) kept->cpChanges = gpu.cpChanges;
        }
        if (!valid) {
            if (kept) eraseDecoded(decodedLists.find(address));
            if (decodedLists.size() >= 1024) clearDecoded();
            DecodedList decoded;
            if (!decodeList(gpu, list, size, decoded)) return false;
            // Its pages are marked before it is read for good, as a mesh's are.
            for (const auto& r : decoded.range) markPages(r[0], r[1]);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (!decodeList(gpu, list, size, decoded)) return false;
            decoded.address = address;
            kept = &decodedLists.emplace(address, std::move(decoded)).first->second;
            decodedIndex[decodedSlot(address)] = kept;
        }
        const DecodedList& d = *kept;
        for (const auto& p : d.primitives) {
            perf.listVertices += p.count, perf.refusedVertices[GxVertexPipeline::MeshSmall] += p.count;
            draw(gpu, p.kind, p.format, p.count, nullptr, &d.vertices[p.first]);
        }
        return true;
    }

    // A list of primitives alone, read into `out`; false for anything else (it stays the command processor's).
    static bool decodeList(const GxCommandProcessor& gpu, const std::uint8_t* list, std::uint32_t size, DecodedList& out) {
        out = DecodedList{};
        const std::uint8_t* lo = reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX);
        const std::uint8_t* hi = nullptr;
        std::uint8_t formats = 0;
        std::uint16_t arrays = 0;
        for (std::uint32_t at = 0; at < size;) {
            const std::uint8_t op = list[at];
            if (op == 0x00) {
                ++at;
                continue;
            }
            if (op < 0x80 || op >= 0xA8 || at + 3 > size) return false;
            const std::uint16_t count = static_cast<std::uint16_t>(list[at + 1] << 8 | list[at + 2]);
            const GxVertexLayout& layout = gpu.layout(op & 7);
            if (at + 3 + std::uint32_t(count) * layout.size > size) return false;
            out.primitives.push_back({static_cast<std::uint8_t>(op & 0xF8), static_cast<std::uint8_t>(op & 7), static_cast<std::uint16_t>(out.vertices.size()), count});
            for (std::uint16_t i = 0; i < count; ++i) {
                const std::uint8_t* vertex = list + at + 3 + i * layout.size;
                out.vertices.push_back(gpu.readVertex(vertex, op & 7));
                layout.indexedSpan(vertex, lo, hi);
            }
            formats = static_cast<std::uint8_t>(formats | 1u << (op & 7));
            arrays = static_cast<std::uint16_t>(arrays | layout.indexedArrays());
            at += 3 + std::uint32_t(count) * layout.size;
        }
        if (out.primitives.empty()) return false;
        for (std::uint8_t reg : {std::uint8_t(0x50), std::uint8_t(0x60)}) out.depends.emplace_back(reg, gpu.cp[reg]);
        for (unsigned f = 0; f < 8; ++f)
            if (formats >> f & 1)
                for (unsigned table : {0x70u, 0x80u, 0x90u}) out.depends.emplace_back(std::uint8_t(table + f), gpu.cp[table + f]);
        for (unsigned a = 0; a < 12; ++a)
            if (arrays >> a & 1)
                for (unsigned base : {0xA0u, 0xB0u}) out.depends.emplace_back(std::uint8_t(base + a), gpu.cp[base + a]);
        out.size = size, out.cpChanges = gpu.cpChanges;
        std::memcpy(out.bytes, list, size);
        out.range[0][0] = reinterpret_cast<std::uintptr_t>(list), out.range[0][1] = out.range[0][0] + size;
        if (hi > lo) out.range[1][0] = reinterpret_cast<std::uintptr_t>(lo), out.range[1][1] = reinterpret_cast<std::uintptr_t>(hi);
        return true;
    }

    // 0 when the list was drawn from a mesh, else why not.
    unsigned displayListMesh(const GxCommandProcessor& gpu, std::uint32_t address, std::uint32_t size) {
        const std::uint8_t* list = gpu.memoryFunction()(address);
        if (!list || size < 3) return GxVertexPipeline::MeshUnsupported;
        if (list[0] < 0x80 || list[0] >= 0xA8) return ++perf.refusedOpcode[list[0]], ++perf.refused[GxVertexPipeline::MeshCommand], GxVertexPipeline::MeshCommand;
        // A list in one 32-byte block (a particle's quad) is a draw of its own as a mesh; read, it joins its neighbors' draw.
        if (size <= 32) return ++perf.refused[GxVertexPipeline::MeshSmall], GxVertexPipeline::MeshSmall;
        applyChangedMemory();
        ensureState(gpu, false);
        bool prepared;
        {
            const Step step(perf, 2);
            prepared = pipeline.prepare(gpu, *state_, frame, 0x90, 0);
        }
        if (!prepared || state_->leftOut) return ++perf.refused[GxVertexPipeline::MeshUnsupported], GxVertexPipeline::MeshUnsupported;
        if (scissorEmpty) return GxVertexPipeline::MeshEmpty;
        if (const unsigned why = pipeline.meshEligible()) return ++perf.refused[why], why;
        const u64 lookup = stepStart();
        gxZone = ZoneMeshLookup;
        // The key: the list, its first and last words (another list at a freed address differs), where the color comes
        // from and the texgens' sources. The CP registers its vertices were read under are checked apart, only those it
        // uses: other formats' tables and the matrix arrays (12-15) change every frame without touching it.
        u32 key[7];
        unsigned n = 0;
        key[n++] = address, key[n++] = size, key[n++] = pipeline.meshColorFromVertex(), key[n++] = pipeline.meshSources();
        key[n++] = pipeline.meshLit();  // lit meshes number their slots another way
        std::memcpy(&key[n], list, 4), std::memcpy(&key[n + 1], list + size - 4, 4), n += 2;
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (unsigned i = 0; i < n; ++i) hash = (hash ^ key[i]) * 0x100000001B3ull;
        CachedMesh* m = meshIndex[u32(hash) % MeshSlots];
        if (!m || m->hash != hash) {
            const auto found = meshes.find(hash);
            m = found == meshes.end() ? nullptr : &found->second;
            if (m) meshIndex[u32(hash) % MeshSlots] = m;
        }
        bool stale = m && (m->key.size() != n || !sameWords(m->key.data(), key, n));
        if (stale) ++perf.mismatchWhy[0];
        bool moved = false;
        if (m && !stale)
            for (auto& [reg, value] : m->depends)
                if (gpu.cp[reg] != value && !stale) {
                    if (reg >= 0xA0 && reg < 0xB0) moved = true, value = gpu.cp[reg];  // where its arrays are: decoded again below
                    else stale = true, ++perf.mismatchWhy[reg < 0x70 ? 1 : reg < 0xA0 ? 2 : 4];
                }
        if (moved && !stale) {
            ++perf.mismatchWhy[3];
            m->moving = true, m->range[1][0] = m->range[1][1] = 0;  // its arrays' writes no longer drop it: it is decoded each frame
        }
        if (stale) dropMesh(meshes.find(hash)), m = nullptr, ++perf.meshMismatches;
        stepEnd(8, lookup);
        gxZone = ZoneListCheck;
        if (!m) {
            if (const auto refused = budgetRefused.find(hash); refused != budgetRefused.end()) {
                if (!makeRoom(refused->second)) return ++perf.refused[GxVertexPipeline::MeshBudget], GxVertexPipeline::MeshBudget;
                budgetRefused.erase(refused);
            }
            // Its pages are marked before it is read: a write and flush from another thread between a reading and the
            // marking would never reach it (the flush hook passes unmarked pages by).
            const std::uint8_t *lo, *hi;
            if (!pipeline.meshSpan(list, size, lo, hi)) return ++perf.refused[GxVertexPipeline::MeshCommand], GxVertexPipeline::MeshCommand;
            markPages(reinterpret_cast<std::uintptr_t>(list), reinterpret_cast<std::uintptr_t>(list) + size);
            markPages(reinterpret_cast<std::uintptr_t>(lo), reinterpret_cast<std::uintptr_t>(hi));
            std::atomic_thread_fence(std::memory_order_seq_cst);
            GxMesh mesh;
            const Step building(perf, 12);
            if (const unsigned why = pipeline.buildMesh(list, size, mesh)) return ++perf.refused[why], why;
            static const bool checking = smgDevGxCheck();
            if (checking) checkMesh(gpu, list, size, mesh, address);
            const std::size_t vertexBytes = mesh.vertices.size(), bytes = vertexBytes + mesh.indices.size() * sizeof(std::uint16_t);
            if (!makeRoom(bytes)) {
                if (budgetRefused.size() > 1024) budgetRefused.clear();
                budgetRefused[hash] = bytes;
                return ++perf.meshBudgetDrops, ++perf.refused[GxVertexPipeline::MeshBudget], GxVertexPipeline::MeshBudget;
            }
            void* block = linearAlloc(bytes);
            if (!block && !meshes.empty()) dropAllMeshes(), block = linearAlloc(bytes);
            if (!block) return GxVertexPipeline::MeshTooLarge;
            std::memcpy(block, mesh.vertices.data(), vertexBytes);
            std::memcpy(static_cast<std::uint8_t*>(block) + vertexBytes, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint16_t));
            GSPGPU_FlushDataCache(block, bytes);
            CachedMesh& cached = meshes[hash];
            cached.hash = hash, meshIndex[u32(hash) % MeshSlots] = &cached;
            cached.key.assign(key, key + n);
            cached.depends.clear();
            for (std::uint8_t reg : {std::uint8_t(0x50), std::uint8_t(0x60)}) cached.depends.emplace_back(reg, gpu.cp[reg]);
            std::uint16_t arrays = 0;
            for (unsigned f = 0; f < 8; ++f) {
                if (!(mesh.formats >> f & 1)) continue;
                for (unsigned table : {0x70u, 0x80u, 0x90u}) cached.depends.emplace_back(std::uint8_t(table + f), gpu.cp[table + f]);
                arrays = static_cast<std::uint16_t>(arrays | gpu.layout(std::uint8_t(f)).indexedArrays());
            }
            for (unsigned a = 0; a < 12; ++a)
                if (arrays >> a & 1)
                    for (unsigned base : {0xA0u, 0xB0u}) cached.depends.emplace_back(std::uint8_t(base + a), gpu.cp[base + a]);
            cached.block = cached.vertices = block, cached.vertexCount = u32(mesh.vertexCount()), cached.indexCount = u32(mesh.indices.size());
            cached.layout = mesh.layout, cached.loaders = loadersFor(mesh.layout);
            cached.origin = std::move(mesh.origin);
            cached.vertexPhysical = osConvertVirtToPhys(block), cached.indexPhysical = osConvertVirtToPhys(static_cast<std::uint8_t*>(block) + vertexBytes);
            cached.slots = mesh.slots;
            cached.groups = std::move(mesh.groups);
            float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
            for (u32 v = 0; v < cached.vertexCount; ++v) {
                float position[3];
                std::memcpy(position, static_cast<const std::uint8_t*>(block) + v * mesh.layout.stride, sizeof position);
                for (int c = 0; c < 3; ++c) low[c] = std::min(low[c], position[c]), high[c] = std::max(high[c], position[c]);
            }
            for (int c = 0; c < 3; ++c) cached.center[c] = cached.vertexCount ? (low[c] + high[c]) / 2 : 0;
            // Apart: a list and its arrays sit in different heaps, and one span over both would take in whatever lies between.
            cached.range[0][0] = reinterpret_cast<std::uintptr_t>(list), cached.range[0][1] = cached.range[0][0] + size;
            cached.range[1][0] = reinterpret_cast<std::uintptr_t>(mesh.arraysLo), cached.range[1][1] = reinterpret_cast<std::uintptr_t>(mesh.arraysHi);
            if (cached.range[1][1] <= cached.range[1][0]) cached.range[1][0] = cached.range[1][1] = 0;  // all attributes direct
            for (const auto& r : cached.range) markPages(r[0], r[1]);
            meshBytes += bytes, ++perf.meshesMade, perf.meshVerticesMade += u32(mesh.vertexCount());
            m = &cached;
        }
        m->lastFrame = framesShown;
        if (m->moving && (moved || m->refreshedFrame != framesShown)) {
            gxZone = ZoneMeshRefresh;
            const bool refreshed = refreshMesh(*m, list, size);
            gxZone = ZoneListCheck;
            if (!refreshed) return dropMesh(meshes.find(hash)), ++perf.refused[GxVertexPipeline::MeshMoving], GxVertexPipeline::MeshMoving;
        }
        drawMesh(*m);
        return GxVertexPipeline::MeshOk;
    }

    // Refreshed vertices live a frame and the next (the GPU may still be drawing them): one arena a frame's parity, taken
    // from the start again when its frame comes round.
    static constexpr std::size_t RefreshArena = 1u << 20;
    std::uint8_t* refreshArena[2] = {};
    std::size_t refreshUsed[2] = {};
    u32 refreshFrame[2] = {~0u, ~0u};
    std::uintptr_t refreshLo = UINTPTR_MAX, refreshHi = 0;  // refreshed since the last command list, flushed with it
    bool refreshMesh(CachedMesh& m, const std::uint8_t* list, std::uint32_t size) {
        const std::size_t bytes = (m.vertexCount * m.layout.stride + 15) & ~std::size_t(15);
        const unsigned k = framesShown & 1;
        if (!refreshArena[k] && !(refreshArena[k] = static_cast<std::uint8_t*>(linearAlloc(RefreshArena)))) return false;
        if (refreshFrame[k] != framesShown) refreshFrame[k] = framesShown, refreshUsed[k] = 0;
        if (refreshUsed[k] + bytes > RefreshArena) return false;
        std::uint8_t* into = refreshArena[k] + refreshUsed[k];
        refreshUsed[k] += bytes;
        if (!pipeline.refreshMesh(list, size, m.layout, m.origin.data(), static_cast<const std::uint8_t*>(m.block), into, m.vertexCount)) return false;
        static const bool checking = smgDevGxCheck();
        if (checking) checkRefresh(list, size, m, into);
        refreshLo = std::min(refreshLo, reinterpret_cast<std::uintptr_t>(into));
        refreshHi = std::max(refreshHi, reinterpret_cast<std::uintptr_t>(into) + bytes);
        m.vertices = into, m.vertexPhysical = osConvertVirtToPhys(into), m.refreshedFrame = framesShown;
        ++perf.meshesRefreshed, perf.meshVerticesRefreshed += m.vertexCount;
        return true;
    }

    void markPages(std::uintptr_t lo, std::uintptr_t hi) { markPages(lo, hi, meshPages); }
    void markPages(std::uintptr_t lo, std::uintptr_t hi, std::unordered_set<std::uintptr_t>& pages) {
        if (hi <= lo) return;
        for (std::uintptr_t page = lo >> 12; page <= (hi - 1) >> 12; ++page) {
            pages.insert(page);
            sMeshPageBits[page >> 5].fetch_or(1u << (page & 31), std::memory_order_relaxed);
        }
    }

    // dev.gx_check: a refresh against the mesh built anew from the list, which it must match bit for bit.
    void checkRefresh(const std::uint8_t* list, std::uint32_t size, const CachedMesh& m, const std::uint8_t* got) {
        ++perf.refreshesChecked;
        GxMesh rebuilt;
        const char* why = pipeline.buildMesh(list, size, rebuilt) ? "the list no longer builds"
                          : rebuilt.vertexCount() != m.vertexCount || rebuilt.layout != m.layout || rebuilt.origin != m.origin ? "the list builds other vertices"
                                                                                                                               : nullptr;
        std::size_t j = 0;
        const std::size_t stride = m.layout.stride;
        for (; !why && j < m.vertexCount; ++j)
            for (std::size_t byte = 0; !why && byte < stride; ++byte)
                if (got[j * stride + byte] != rebuilt.vertices[j * stride + byte]) why = m.layout.fieldAt(static_cast<unsigned>(byte));
        if (!why) return;
        if (++perf.refreshesDiffering <= 4)
            std::fprintf(stderr, "gx check: a refreshed mesh (%lu vertices) differs from its rebuild: %s (vertex %zu)\n",
                         static_cast<unsigned long>(m.vertexCount), why, j ? j - 1 : 0);
    }

    // dev.gx_check: a new mesh against the CPU path, vertex by vertex, under the state it is first drawn with (each
    // group's uniforms through gxMeshModel); a mesh's first difference is logged.
    void checkMesh(const GxCommandProcessor& gpu, const std::uint8_t* list, std::uint32_t size, const GxMesh& mesh, std::uint32_t address) {
        ++perf.meshesChecked;
        std::vector<GxMeshUniforms> uniforms(mesh.groups.size());
        pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], uniforms[0]);
        for (std::size_t g = 1; g < mesh.groups.size(); ++g) uniforms[g] = uniforms[0], pipeline.meshSlotUniforms(mesh.groups[g], uniforms[g]);
        std::vector<u32> group(mesh.vertexCount(), 0);
        for (u32 g = 0; g < mesh.groups.size(); ++g)
            for (u32 i = mesh.groups[g].firstIndex; i < mesh.groups[g].firstIndex + mesh.groups[g].indexCount; ++i) group[mesh.indices[i]] = g;
        std::vector<GxDrawVertex> cpu, drawn;
        std::vector<std::uint16_t> indices;
        std::vector<u32> corners;  // the list's triangles, as buildMesh makes them, by list vertex
        for (u32 at = 0; at < size;) {
            const std::uint8_t op = list[at];
            if (!op) {
                ++at;
                continue;
            }
            const std::uint16_t count = static_cast<std::uint16_t>(list[at + 1] << 8 | list[at + 2]);
            if (!pipeline.prepare(gpu, *state_, frame, op & 0xF8, op & 7)) return void(std::fprintf(stderr, "gx check: the CPU path refuses a mesh's primitive\n"));
            drawn.resize(count), indices.resize(GxVertexPipeline::maxIndices(count));
            pipeline.run(list + at + 3, count, drawn.data(), 0, indices.data());
            const u32 first = u32(cpu.size());
            const auto triangle = [&](u32 a, u32 b, u32 c) { corners.insert(corners.end(), {first + a, first + b, first + c}); };
            switch (op & 0xF8) {
            case 0x80: case 0x88:
                for (u32 i = 0; i + 3 < count; i += 4) triangle(i, i + 1, i + 2), triangle(i, i + 2, i + 3);
                break;
            case 0x90:
                for (u32 i = 0; i + 2 < count; i += 3) triangle(i, i + 1, i + 2);
                break;
            case 0x98:
                for (u32 i = 0; i + 2 < count; ++i) i & 1 ? triangle(i + 1, i, i + 2) : triangle(i, i + 1, i + 2);
                break;
            case 0xA0:
                for (u32 i = 1; i + 1 < count; ++i) triangle(0, i, i + 1);
                break;
            }
            cpu.insert(cpu.end(), drawn.begin(), drawn.end());
            at += 3 + u32(count) * gpu.layout(op & 7).size;
        }
        bool sameTriangles = corners.size() == mesh.indices.size();
        for (std::size_t i = 0; sameTriangles && i < corners.size(); ++i) sameTriangles = mesh.indices[i] < mesh.origin.size() && mesh.origin[mesh.indices[i]] == corners[i];
        u32 drawnIndices = 0;
        for (const GxMesh::Group& g : mesh.groups) sameTriangles &= g.firstIndex == drawnIndices, drawnIndices += g.indexCount;
        if (!sameTriangles || drawnIndices != mesh.indices.size()) {
            ++perf.meshesDiffering;
            return void(std::fprintf(stderr, "gx check: mesh at 0x%08lx (%zu groups) has triangles other than its list's\n", static_cast<unsigned long>(address),
                                     mesh.groups.size()));
        }
        pipeline.prepare(gpu, *state_, frame, 0x90, 0);
        const auto off = [](float got, float want) { return std::fabs(got - want) > 1e-3f * std::max(1.0f, std::fabs(want)); };
        for (std::size_t j = 0; j < mesh.vertexCount(); ++j) {
            if (mesh.origin[j] >= cpu.size()) return void(std::fprintf(stderr, "gx check: a mesh vertex from past its list's\n"));
            const GxMeshVertex vertex = mesh.vertex(j);
            const GxDrawVertex& want = cpu[mesh.origin[j]];
            const GxMeshUniforms& u = uniforms[group[j]];
            float clip[4], color[4], tex[CombinerProgram::MaxUnits][2];
            gxMeshModel(u, vertex, clip, color, tex);
            const char* what = nullptr;
            // Against w, as the screen sees it: a far mesh's rounding is large in clip units and far below a pixel.
            for (int r = 0; r < 4 && !what; ++r)
                if (std::fabs(clip[r] - want.position[r]) > 1e-4f * std::max({1.0f, std::fabs(want.position[3]), std::fabs(want.position[r])}))
                    what = "position";
            for (int c = 0; c < 4 && !what; ++c)
                if (std::abs(int(std::lround(color[c] * 255)) - int(want.color[c])) > 2) what = "color";
            for (unsigned t = 0; t < u.units && !what; ++t)
                if (off(tex[t][0], want.texcoord[t][0]) || off(tex[t][1], want.texcoord[t][1])) what = "texture coordinates";
            if (!what) continue;
            ++perf.meshesDiffering;
            const GxMesh::Group& in = mesh.groups[group[j]];
            std::fprintf(stderr, "gx check: mesh at 0x%08lx (%zu vertices, %zu groups, lit %d, %u light entries, %u units, slots 0x%x) vertex %zu "
                         "(list vertex %lu, group %lu of slots %u %u %u %u, slot row %.0f) differs in %s: position %.3f %.3f %.3f %.3f vs %.3f %.3f "
                         "%.3f %.3f; color %.0f %.0f %.0f %.0f vs %u %u %u %u; first texcoords %.3f %.3f vs %.3f %.3f; normal %.3f %.3f %.3f\n",
                         static_cast<unsigned long>(address), mesh.vertexCount(), mesh.groups.size(), u.lit, u.lights, u.units, mesh.slots, j,
                         static_cast<unsigned long>(mesh.origin[j]), static_cast<unsigned long>(group[j]), in.slotMap[0], in.slotMap[1], in.slotMap[2],
                         in.slotMap[3], vertex.slot, what, clip[0], clip[1], clip[2], clip[3], want.position[0], want.position[1],
                         want.position[2], want.position[3], color[0] * 255, color[1] * 255, color[2] * 255, color[3] * 255, want.color[0], want.color[1],
                         want.color[2], want.color[3], tex[0][0], tex[0][1], want.texcoord[0][0], want.texcoord[0][1], vertex.normal[0],
                         vertex.normal[1], vertex.normal[2]);
            return;
        }
    }

    // The PICA carries texture coordinates as 24-bit floats, and at a few thousand they lose whole texels (the castle
    // lake's ripples, generated from world position): a repeating texture's move by whole periods to stay small.
    void keepTexgensSmall(const CachedMesh& mesh, GxMeshUniforms& u) const {
        for (unsigned unit = 0; unit < u.units; ++unit) {
            const C3D_Tex* texture = sent.bound[unit];
            if (!texture || u.projective[unit] || u.slotTexgen[unit]) continue;
            const GxMeshLayout::Unit& input = mesh.layout.unit[unit];
            const float* at = input.input == GxMeshLayout::OfPosition ? mesh.center : nullptr;  // other inputs stay near zero
            for (int axis = 0; axis < 2; ++axis) {
                const unsigned wrap = texture->param >> (axis ? 8 : 12) & 3;
                const float period = wrap == GPU_REPEAT ? 1 : wrap == GPU_MIRRORED_REPEAT ? 2 : 0;
                if (!period) continue;
                float* row = u.texgen[unit][axis];
                float value = row[3];
                if (at) value += row[0] * at[0] + row[1] * at[1] + (input.floats == 3 ? row[2] * at[2] : 0);
                row[3] -= period * std::floor(value / period);
            }
        }
    }

    void drawMesh(const CachedMesh& mesh) {
        drawing = true;
        const struct Release { bool& flag; ~Release() { flag = false; } } release{drawing};
        if (!inFrame) begin();
        flushBatch();
        roomFor(0, DrawCommandWords + u32(mesh.groups.size() - 1) * GroupCommandWords);
        applyState();
        const GxZoneScope zone(ZoneUniforms);
        const u64 uniforms = stepStart();
        GxMeshUniforms u;
        pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], u);
        keepTexgensSmall(mesh, u);
        if (dumping) dumpMesh(mesh, u);
        if (u.reject == 3) return;  // GX culls every triangle
        slotUniforms(u);
        if (u.lit && lightingRowsEpoch != pipeline.lightingEpoch()) {  // the lighting's rows are made of nothing else
            lightingRowsEpoch = pipeline.lightingEpoch();
            for (unsigned l = 0; l < u.lights; ++l)
                for (int r = 0; r < 6; ++r) {
                    const float* light = u.light[l][r];
                    row(lightUniform + int(l) * 6 + r, light[0], light[1], light[2], light[3]);
                }
            row(ambientUniform, u.ambient[0], u.ambient[1], u.ambient[2], u.ambient[3]);
            integer(lightLoopUniform, u.lights ? int(u.lights) - 1 : 0, 0, 6, 0);
            boolean(anyLightUniform, u.lights != 0);
            boolean(rgbLitUniform, u.rgbLit);
            boolean(alphaLitUniform, u.alphaLit);
            boolean(ambientRgbUniform, u.rgbAmbientFromVertex);
            boolean(ambientAlphaUniform, u.alphaAmbientFromVertex);
        }
        for (unsigned unit = 0; unit < u.units; ++unit) {
            for (int r = 0; r < 3; ++r) {
                const float* texrow = u.texgen[unit][r];
                row(texrowUniform + int(unit) * 3 + r, texrow[0], texrow[1], texrow[2], texrow[3]);
            }
        }
        if (u.slotTexgen[0] | u.slotTexgen[1] | u.slotTexgen[2]) row(slotRowUniform, u.slotTexgenRow[0], u.slotTexgenRow[1], u.slotTexgenRow[2], 0);
        // The bools every draw sets (lit, each unit's projection, each slot texgen, transform, the colors' registers), in one compare.
        u16 boolMask = boolBit(litUniform) | boolBit(transformUniform) | boolBit(rgbRegUniform) | boolBit(alphaRegUniform);
        u16 boolValue = u16((u.lit ? boolBit(litUniform) : 0) | boolBit(transformUniform) | (!u.fixedColor && !u.rgbFromVertex ? boolBit(rgbRegUniform) : 0) |
                            (!u.fixedColor && !u.alphaFromVertex ? boolBit(alphaRegUniform) : 0));
        for (unsigned unit = 0; unit < 3; ++unit) {
            boolMask |= boolBit(slotGenUniform[unit]);
            if (unit < u.units && u.slotTexgen[unit]) boolValue |= boolBit(slotGenUniform[unit]);
            if (unit >= u.units) continue;
            boolMask |= boolBit(projectUniform[unit]);
            if (u.projective[unit]) boolValue |= boolBit(projectUniform[unit]);
        }
        booleans(boolMask, boolValue);
        row(materialUniform, u.color[0] / 255.0f, u.color[1] / 255.0f, u.color[2] / 255.0f, u.color[3] / 255.0f);
        stepEnd(9, uniforms);
        gxZone = ZoneMeshDraw;
        const u64 drawing = stepStart();
        // The vertex shader's inputs are position (v0), color (v1), three texgen inputs (v2-v4), normal (v6) and slot (v5); what
        // the mesh doesn't store is a fixed attribute. The loaders depend only on the layout, so consecutive meshes alike keep citro3d's.
        MeshLoaders& loaders = meshLoaders[mesh.loaders];
        if (sent.meshLayout != mesh.loaders) {
            C3D_SetAttrInfo(&loaders.attributes);
            sent.meshLayout = mesh.loaders, pend(PendingMeshLayout);
            meshAttributes = loaders.attributes;
        }
        // A fixed value goes to its attribute's place among the seven (color 1, texgen inputs 2-4, normal 5, slot 6), which
        // citro3d numbers in the order they were added, not to the shader register it feeds.
        const GxMeshLayout& layout = mesh.layout;
        if (layout.color == GxMeshLayout::Absent) sendFixed(1, u.color[0], u.color[1], u.color[2], u.color[3]);
        for (unsigned unit = 0; unit < 3; ++unit)
            if (layout.unit[unit].input == GxMeshLayout::Unused) sendFixed(2 + int(unit), 0, 0, 0, 1);
        if (!layout.lit) sendFixed(5, 0, 0, 0, 0);
        if (layout.slot == GxMeshLayout::Absent) sendFixed(6, layout.lit ? 4 : 0, 0, 0, 0);  // the one slot's first row
        // GX culls in the EFB, its y down; the PICA sees the same triangles turned for the screen.
        sendCull(u.reject == 1 ? GPU_CULL_FRONT_CCW : u.reject == 2 ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);
        const auto* indices = reinterpret_cast<const std::uint16_t*>(static_cast<std::uint8_t*>(mesh.block) + mesh.vertexCount * layout.stride);
        const auto drawGroups = [&](bool again) {
            for (std::size_t g = 0; g < mesh.groups.size(); ++g) {
                if (g || (again && mesh.groups.size() > 1)) pipeline.meshSlotUniforms(mesh.groups[g], u), slotUniforms(u);
                static const bool noRaw = smgDevGxNoRaw();
                if (noRaw) pending = true;
                const bool viaCitro3d = pending && (noRaw || (pendingWhy & ~RawBlocks));
                if (pending && !viaCitro3d) writeChangedBlocks();
                sendUniforms();
                if (viaCitro3d) {
                    C3D_BufInfo* buffers = C3D_GetBufInfo();
                    BufInfo_Init(buffers);
                    for (int b = 0; b < loaders.bufferCount; ++b)
                        BufInfo_Add(buffers, static_cast<const std::uint8_t*>(mesh.vertices) + loaders.buffers[b].offset, layout.stride, loaders.buffers[b].components,
                                    loaders.buffers[b].permutation);
                    sent.aliasBuffers |= loaders.bufferCount > 1;
                    C3D_DrawElements(GPU_TRIANGLES, int(mesh.groups[g].indexCount), C3D_UNSIGNED_SHORT, indices + mesh.groups[g].firstIndex);
                    for (unsigned why = 0; why <= PendingList; ++why) perf.pendingWhy[why] += pendingWhy >> why & 1;
                    sentAll(), ++perf.citroDraws;
                } else {
                    writeMeshDraw(mesh, mesh.groups[g], g == 0);
                }
            }
        };
        drawGroups(false);
        if (!state_->passes.empty()) drawSharedPasses([&] { drawGroups(true); });
        u32* buffer;
        u32 size, offset;
        GPUCMD_GetBuffer(&buffer, &size, &offset);
        if (size - offset < 0x40) stop("a mesh's draws filling the GPU's command list");  // a raw write past its room
        cpuBound = false;
        fresh[0] = fresh[1] = fresh[2] = false;
        ++perf.meshDraws, perf.meshTriangles += mesh.indexCount / 3;
        stepEnd(10, drawing);
    }

    // Each slot's rows: four of the clip matrix unlit; lit, view to clip in the first four and six (modelview, normal)
    // for each compact slot after them.
    // The pipeline's epochs when rows 0-3 and the lighting's rows were last set: only slotUniforms and drawMesh write them.
    u32 toClipRowsEpoch = ~0u, lightingRowsEpoch = ~0u;
    void slotUniforms(const GxMeshUniforms& u) {
        if (!u.lit) {
            toClipRowsEpoch = ~0u;  // the first slot's clip rows are these rows
            for (unsigned slot = 0; slot < 10; ++slot) {
                if (!(u.slots >> slot & 1)) continue;
                for (int r = 0; r < 4; ++r) {
                    const float* clip = u.clip[slot] + r * 4;
                    row(clipUniform + int(slot) * 4 + r, clip[0], clip[1], clip[2], clip[3]);
                }
            }
            return;
        }
        if (toClipRowsEpoch != pipeline.toClipEpoch()) {
            toClipRowsEpoch = pipeline.toClipEpoch();
            for (int r = 0; r < 4; ++r) row(clipUniform + r, u.toClip[r * 4], u.toClip[r * 4 + 1], u.toClip[r * 4 + 2], u.toClip[r * 4 + 3]);
        }
        const int paired = (u.slotRows - 6) / 3;
        for (unsigned slot = 0; slot < GxMesh::MaxLitSlots; ++slot) {
            if (!(u.slots >> slot & 1)) continue;
            const int at = clipUniform + 4 + int(slot) * u.slotRows;
            for (int r = 0; r < 3; ++r) {
                const float* modelView = u.modelView[slot] + r * 4;
                row(at + r, modelView[0], modelView[1], modelView[2], modelView[3]);
                const float* normal = u.normal[slot] + r * 3;
                row(at + 3 + r, normal[0], normal[1], normal[2], 0);
                for (int p = 0; p < paired; ++p) {
                    const float* texture = u.paired[slot][p] + r * 4;
                    row(at + 6 + 3 * p + r, texture[0], texture[1], texture[2], texture[3]);
                }
            }
        }
    }

    // The pending primitives as one draw, under the state they share.
    void flushBatch() {
        if (indexUsed == batchStart) return;
        const Step step(perf, 7);
        drawing = true;
        const struct Release { bool& flag; ~Release() { flag = false; } } release{drawing};
        applyState();
        const bool wholeScreen = dumping && dumpBatch();
        ensureCpuBuffers();
        sendCull(GPU_CULL_NONE);  // culled in EFB space already
        C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(indexUsed - batchStart), C3D_UNSIGNED_SHORT, indices + batchStart);
        if (!state_->passes.empty())
            drawSharedPasses([&] { C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(indexUsed - batchStart), C3D_UNSIGNED_SHORT, indices + batchStart); });
        if (passIndicesWaiting) drawWaitingPasses();
        sentAll();
        ++perf.draws, perf.triangles += (indexUsed - batchStart) / 3;
        batchStart = indexUsed;
        fresh[0] = fresh[1] = fresh[2] = false;
        if (wholeScreen) dumpEfbAverage("after it");
    }

    // A pass's pixel state: its program, under the material's tests, its color added on and the EFB's alpha kept.
    const PicaPixel& passPixel(std::size_t i) {
        if (stateEntry->passPixels.size() != state_->passes.size()) {
            stateEntry->passPixels.clear();
            for (const auto& pass : state_->passes) {
                PicaPixel p = preparePixel(state_->material, pass.program);
                if (pass.blend == TevPass::Retest) {  // blended as the material is, under a compare of its own
                    p.alphaTest = pass.compare != 7, p.alphaFunc = testFunction(pass.compare), p.alphaReference = pass.reference;
                    stateEntry->passPixels.push_back(p);
                    continue;
                }
                p.equation = p.alphaEquation = GPU_BLEND_ADD;
                const bool byAlpha = pass.blend == TevPass::AddTimesAlpha || pass.blend == TevPass::Over;
                p.source = byAlpha ? GPU_SRC_ALPHA : GPU_ONE, p.destination = pass.blend == TevPass::Over ? GPU_ONE_MINUS_SRC_ALPHA : GPU_ONE;
                p.alphaSource = pass.writesAlpha ? GPU_ONE : GPU_ZERO, p.alphaDestination = pass.writesAlpha ? GPU_ZERO : GPU_ONE;
                if (pass.blend == TevPass::AddWhereEqual) p.alphaTest = true, p.alphaFunc = GPU_EQUAL, p.alphaReference = pass.reference;
                if (pass.blend == TevPass::Over) p.alphaTest = false;
                if (pass.blend == TevPass::Sub) p.equation = GPU_BLEND_REVERSE_SUBTRACT;  // the rest less the pass
                if (pass.blend == TevPass::Scale) p.source = GPU_ZERO, p.destination = GPU_SRC_COLOR;
                if (pass.blend == TevPass::Gain) p.source = GPU_DST_COLOR;
                stateEntry->passPixels.push_back(p);
            }
        }
        return stateEntry->passPixels[i];
    }

    // The passes that sample what the program does, over the triangles just drawn (a batch's, or a mesh group's through
    // `draw`), the EFB's depth kept.
    template <class Draw> void drawSharedPasses(Draw draw) {
        const PicaPixel* const own = pixelNow;
        const auto ownMask = writeMask;
        for (std::size_t i = 0; i < state_->passes.size(); ++i) {
            if (!state_->passes[i].sameUnits) continue;
            pixelNow = &passPixel(i), writeMask = ownMask & ~GPU_WRITE_DEPTH;
            sendPixel();
            draw();
            ++perf.passDraws;
        }
        pixelNow = own, writeMask = ownMask;
        applied = false;  // the state's own pixel goes back on with its next draw
    }

    // The other passes: such a pass samples other coordinates than the program, so each primitive's vertices are made again
    // under its program, beside the program's, and its indices wait in passIndices until the batch is drawn. A state each,
    // as prepare() knows a state it has seen by its address.
    std::vector<GxDrawState> passStates;
    std::vector<std::vector<std::uint16_t>> passIndices;
    std::size_t passIndicesWaiting = 0;
    void stagePasses(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* data,
                     const GxVertex* decoded, const GxDrawVertex* program) {
        passStates.resize(state_->passes.size()), passIndices.resize(state_->passes.size());
        for (std::size_t i = 0; i < state_->passes.size(); ++i) {
            const GxDrawState::Pass& pass = state_->passes[i];
            if (pass.sameUnits) continue;
            GxDrawState& passState = passStates[i];
            passState = *state_, passState.passes.clear(), passState.program = pass.program;
            if (!pipeline.prepare(gpu, passState, frame, kind, format)) stop(pipeline.unsupported);
            std::vector<std::uint16_t>& waiting = passIndices[i];
            const std::size_t at = waiting.size();
            waiting.resize(at + GxVertexPipeline::maxIndices(kind, count));
            const auto first = static_cast<std::uint16_t>(used);
            // The pass draws where the program did, its colors the same: only its coordinates are new.
            std::size_t written = 0;
            if (!pipeline.runPass(data, decoded, count, program, vertices + used, first, waiting.data() + at, written))
                written = decoded ? pipeline.runDecoded(decoded, count, vertices + used, first, waiting.data() + at)
                                  : pipeline.run(data, count, vertices + used, first, waiting.data() + at);
            waiting.resize(at + written), used += pipeline.verticesWritten, passIndicesWaiting += written;
        }
    }
    void drawWaitingPasses() {
        const PicaPixel* const own = pixelNow;
        const auto ownMask = writeMask;
        for (std::size_t i = 0; i < passIndices.size(); ++i) {
            std::vector<std::uint16_t>& waiting = passIndices[i];
            if (waiting.empty()) continue;
            const GxDrawState::Pass& pass = state_->passes[i];
            std::copy(waiting.begin(), waiting.end(), indices + indexUsed);
            pixelNow = &passPixel(i), writeMask = ownMask & ~GPU_WRITE_DEPTH;
            sendPixel();
            for (unsigned unit = 0; unit < pass.program.unitCount; ++unit) sendTexture(unit, texture(state_->textures[pass.program.units[unit].texture]));
            C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(waiting.size()), C3D_UNSIGNED_SHORT, indices + indexUsed);
            indexUsed += waiting.size(), waiting.clear();
            ++perf.passDraws;
        }
        passIndicesWaiting = 0;
        pixelNow = own, writeMask = ownMask;
        applied = false;  // the state's own textures go back on with the next batch
    }

    void applyState() {
        const Step step(perf, 1);
        if (!applied || appliedVersion != stateVersion) {
            sendPixel();
            sendFog();
            sendScissor();
            for (unsigned unit = 0; unit < state_->program.unitCount; ++unit) {
                const int map = state_->program.units[unit].texture;
                if (map != CombinerProgram::Palette && boundAtlas >= 0 && unit == stateEntry->mapUnit) sendTexture(unit, atlases[std::size_t(boundAtlas)].texture);
                else if (map != CombinerProgram::Palette) sendTexture(unit, texture(state_->textures[map]));
                else if (batchPalette >= 0) sendTexture(unit, palettes[batchPalette]);
                else stop("the C1 palette's unit with no palette chosen");
            }
            applied = true, appliedVersion = stateVersion;
        }
    }

    // The EFB's average color so far, read back (dumps only): which full-screen pass changes the picture, and how.
    void dumpEfbAverage(const char* when) {
        const u32* pixels = readEfb(false);
        unsigned long long sum[4] = {};
        for (unsigned i = 0; i < 240 * 400; ++i)
            for (int c = 0; c < 4; ++c) sum[c] += pixels[i] >> (24 - 8 * c) & 0xFF;
        std::fprintf(stderr, "gx dump:   EFB %s: average %llu,%llu,%llu,%llu\n", when, sum[0] / 96000, sum[1] / 96000, sum[2] / 96000, sum[3] / 96000);
    }

    // A Wii frame's commands can outgrow citro3d's buffer (or its vertices the port's): the frame is ended and begun
    // again on the same target with its display output held back, so no part-drawn frame is shown.
    // Room for what comes next: GPUCMD_Add drops a command that doesn't fit the list, silently.
    void roomFor(std::size_t vertexCount, u32 words = DrawCommandWords) {
        u32* buffer;
        u32 size, offset;
        GPUCMD_GetBuffer(&buffer, &size, &offset);
        if (size - offset >= words && used + vertexCount <= MaxVertices &&
            indexUsed + passIndicesWaiting + GxVertexPipeline::maxIndices(vertexCount) <= MaxIndices)
            return;
        if (vertexCount > MaxVertices) stop("a primitive with more vertices than the port's buffer");
        pause();
        resume();
    }

    void perfLine() {
        const u64 now = svcGetSystemTick();
        ++perf.frames, ++framesShown;
        if (!perf.since) perf.since = now;
        const double ms = double(now - perf.since) * 1000.0 / SYSCLOCK_ARM11;
        if (ms < 4000) return;
        const double n = perf.frames;
        double phases[Phases];
        const u32 phaseFrames = sPhaseFrames.exchange(0, std::memory_order_relaxed);
        for (int p = 0; p < Phases; ++p)
            phases[p] = phaseFrames ? sPhaseTicks[p].exchange(0, std::memory_order_relaxed) * 1000.0 / SYSCLOCK_ARM11 / phaseFrames : 0;
        GxThreadClock& clock = gxThreadClock();
        const double tick = 1000.0 / SYSCLOCK_ARM11 / n;
        // t= is the emulated clock (drives time their actions by it when Azahar runs faster than real time).
        std::fprintf(stderr, "gx perf: t=%.0f, %lu frames, %.1f ms/frame (%.1f fps, %.2f logic steps each); per logic step: game thread draw %.1f, "
                     "logic %.1f, animation %.1f, waiting for the GPU %.1f, retrace wait %.1f; per frame: GX stream read %s %.1f ms (%.1f of it waiting "
                     "for the PICA), game waiting for room %.1f ms, for the frame before %.1f ms, for all drawn %.1f ms; %.0f KB; DSP %.1f ms; %.0f primitives (%.0f same state, %.0f with only C0 new), %.0f "
                     "vertices, %.0f triangles, %.0f draws; depth peeks %.1f (%.1f answered from the frame before, %.2f ms); atlases %zu (%lu made), %.0f draws joined by them, %.0f turns\n",
                     double(now) / SYSCLOCK_ARM11, static_cast<unsigned long>(perf.frames), ms / n, n * 1000.0 / ms, phaseFrames / n, phases[0], phases[1],
                     phases[2], phases[3], phases[4],
                     clock.core == 2 ? "on core 2" : clock.core == 1 ? "on core 1" : "on the game's thread", clock.busy.exchange(0) * tick,
                     perf.syncTicks * tick, clock.gameWaits.exchange(0) * tick, clock.frameWaits.exchange(0) * tick, clock.drainWaits.exchange(0) * tick,
                     clock.bytes.exchange(0) / 1024.0 / n,
                     smgDspTicks.exchange(0, std::memory_order_relaxed) * tick, perf.primitives / n,
                     perf.sameState / n, perf.color0Rides / n, perf.vertices / n, perf.triangles / n, perf.draws / n, perf.peeks / n,
                     perf.peeksDeferred / n, perf.peekTicks * tick, atlases.size(), static_cast<unsigned long>(perf.atlasesBuilt), perf.atlasJoins / n, perf.atlasTurns / n);
        // The registers behind most state changes, so batching can be aimed at them.
        std::string top;
        for (int k = 0; k < 6; ++k) {
            const u32* most = std::max_element(perf.changed, perf.changed + 0x100);
            if (!*most) break;
            char item[32];
            std::snprintf(item, sizeof item, " %02X:%.0f", unsigned(most - perf.changed), *most / n);
            top += item;
            perf.changed[most - perf.changed] = 0;
        }
        std::fprintf(stderr, "gx perf: GPU path %.0f draws (%.0f groups through citro3d), %.0f triangles, %.1f meshes made per frame (%zu kept, %.1f MB); draw states %zu kept, %lu clears; TEV programs compiled %.1f, "
                     "movie frames decoded %.1f, display lists %.0f (%.0f bytes) per frame; %.1f MB linear free, %.1f MB of %.1f in the free store taken; state changes by BP register "
                     "(per frame):%s\n",
                     perf.meshDraws / n, perf.citroDraws / n, perf.meshTriangles / n, perf.meshesMade / n, meshes.size(), meshBytes / 1048576.0, states.size(),
                     static_cast<unsigned long>(perf.stateClears), (gxTevCompiles() - perf.compilesBefore) / n,
                     (smgMovieDecodes() - perf.moviesBefore) / n, (lastGpu->displayListCalls - perf.listsBefore) / n,
                     (lastGpu->displayListBytes - perf.listBytesBefore) / n, linearSpaceFree() / 1048576.0, mallinfo().uordblks / 1048576.0,
                     __ctru_heap_size / 1048576.0, top.c_str());
        // Why display lists stayed on the CPU path, and the first commands of those that weren't primitives.
        std::string refusals;
        for (unsigned why = 1; why < GxVertexPipeline::MeshRefusals; ++why) {
            char item[24];
            std::snprintf(item, sizeof item, " %u:%.0f", why, perf.refused[why] / n);
            if (perf.refused[why]) refusals += item;
        }
        refusals += "; first commands";
        for (int k = 0; k < 4; ++k) {
            const u32* most = std::max_element(perf.refusedOpcode, perf.refusedOpcode + 0x100);
            if (!*most) break;
            char item[24];
            std::snprintf(item, sizeof item, " %02X:%.0f", unsigned(most - perf.refusedOpcode), *most / n);
            refusals += item;
            perf.refusedOpcode[most - perf.refusedOpcode] = 0;
        }
        if (perf.timed[0] | perf.timed[1] | perf.timed[2] | perf.timed[3] | perf.timed[4] | perf.timed[5]) {
            const double ms = 1000.0 / SYSCLOCK_ARM11 / n;
            std::fprintf(stderr, "gx perf: renderer time per frame: CPU-path primitives %.1f ms immediate + %.1f ms in lists (%.0f vertices), display "
                         "lists %.1f ms, texture copies %.1f ms (%.1f CPU taking %.1f ms, %.1f GPU), frame end %.1f ms, movie %.1f ms\n", perf.timed[0] * ms,
                         perf.timed[4] * ms, perf.listVertices / n, perf.timed[1] * ms, perf.timed[2] * ms, perf.cpuCopies / n, perf.cpuCopyTicks * ms, perf.gpuCopies / n,
                         perf.timed[5] * ms, perf.timed[3] * ms);
            std::fprintf(stderr, "gx perf: of which per frame: building states %.1f ms (%.0f), applying them %.1f ms (%.0f), preparing the vertex "
                         "pipeline %.1f ms (%.0f); building: reading GX %.1f ms (%.0f cached, %.0f read), stats %.1f ms, pixel state %.1f ms\n",
                         perf.stepTicks[0] * ms, perf.steps[0] / n, perf.stepTicks[1] * ms, perf.steps[1] / n, perf.stepTicks[2] * ms, perf.steps[2] / n,
                         perf.stepTicks[3] * ms, perf.stateHits / n, perf.stateMisses / n, perf.stepTicks[4] * ms, perf.stepTicks[5] * ms);
            std::fprintf(stderr, "gx perf: and: CPU-path vertices %.1f ms (%.0f calls), CPU-path draws %.1f ms (%.0f); mesh lookups %.1f ms (%.0f), "
                         "uniforms %.1f ms (%.0f), draw calls %.1f ms (%.0f), first-use textures %.1f ms (%.1f)\n", perf.stepTicks[6] * ms,
                         perf.steps[6] / n, perf.stepTicks[7] * ms, perf.steps[7] / n, perf.stepTicks[8] * ms, perf.steps[8] / n, perf.stepTicks[9] * ms,
                         perf.steps[9] / n, perf.stepTicks[10] * ms, perf.steps[10] / n, perf.stepTicks[11] * ms, perf.steps[11] / n);
            std::fprintf(stderr, "gx perf: meshes made: %.1f ms (%.1f, %.0f vertices)\n", perf.stepTicks[12] * ms, perf.steps[12] / n,
                         perf.meshVerticesMade / n);
            const u32* counts = lastGpu->commandCounts;
            const u32* bytes = lastGpu->commandBytes;
            std::fprintf(stderr, "gx perf: commands per frame (count/KB): BP %.0f/%.0f, CP %.0f/%.0f, XF %.0f/%.0f, XF indexed %.0f/%.0f, list calls %.0f/%.0f, "
                         "primitives %.0f/%.0f, other %.0f/%.0f\n", counts[0] / n, bytes[0] / n / 1024, counts[1] / n, bytes[1] / n / 1024, counts[2] / n,
                         bytes[2] / n / 1024, counts[3] / n, bytes[3] / n / 1024, counts[4] / n, bytes[4] / n / 1024, counts[5] / n, bytes[5] / n / 1024,
                         counts[6] / n, bytes[6] / n / 1024);
            const u32* why = perf.pendingWhy;
            std::fprintf(stderr, "gx perf: mesh draws through citro3d after a change of: combiner %.0f, fragment ops %.0f, cull %.0f, scissor %.0f, "
                         "textures %.0f, fixed attributes %.0f, CPU-path buffers %.0f, mesh layout %.0f, fog %.0f, command list %.0f\n", why[0] / n,
                         why[1] / n, why[2] / n, why[3] / n, why[4] / n, why[5] / n, why[6] / n, why[7] / n, why[8] / n, why[9] / n);
            std::fprintf(stderr, "gx perf: pipeline per frame: %.0f prepares read whole, %.0f folds made, %.0f lighting reads; vertex layouts %.0f read, "
                         "%.0f reused; unfolded texgen vertices: matrix the position's %.0f, its pair %.0f, another %.0f, the last again %.0f, normalized %.0f\n",
                         pipeline.fullPrepares / n, pipeline.foldsMade / n, pipeline.lightingReads / n, lastGpu->layoutsRead / n,
                         lastGpu->layoutsReused / n, pipeline.texMatrixShared / n, pipeline.texMatrixPaired / n, pipeline.texMatrixOwn / n,
                         pipeline.texMatrixRepeated / n, pipeline.texgensNormalized / n);
        }
        pipeline.fullPrepares = pipeline.foldsMade = pipeline.lightingReads = 0;
        pipeline.texMatrixShared = pipeline.texMatrixPaired = pipeline.texMatrixOwn = pipeline.texMatrixRepeated = pipeline.texgensNormalized = 0;
        lastGpu->layoutsRead = lastGpu->layoutsReused = 0;
        std::fill(std::begin(lastGpu->commandCounts), std::end(lastGpu->commandCounts), 0u);
        std::fill(std::begin(lastGpu->commandBytes), std::end(lastGpu->commandBytes), 0u);
        std::string approximations;
        for (const auto& [why, count] : perf.approximated)
            if (why) approximations += std::string(approximations.empty() ? " " : ", ") + why + " " + std::to_string(count);
        if (!approximations.empty()) std::fprintf(stderr, "gx perf: new draw states approximated:%s\n", approximations.c_str());
        std::string listed;
        for (unsigned why = 0; why < GxVertexPipeline::MeshRefusals; ++why)
            if (perf.refusedVertices[why]) listed += " " + std::to_string(why) + ":" + std::to_string(unsigned(perf.refusedVertices[why] / n));
        std::fprintf(stderr, "gx perf: CPU-path vertices in lists refused by reason (per frame):%s\n", listed.c_str());
        if (perf.meshesChecked || perf.refreshesChecked)
            std::fprintf(stderr, "gx perf: meshes checked %lu, differing %lu; refreshes checked %lu, differing %lu\n", static_cast<unsigned long>(perf.meshesChecked),
                         static_cast<unsigned long>(perf.meshesDiffering), static_cast<unsigned long>(perf.refreshesChecked),
                         static_cast<unsigned long>(perf.refreshesDiffering));
        std::fprintf(stderr, "gx perf: display lists refused by reason (per frame):%s; meshes invalidated %.1f, overflows %.1f, over budget %.1f, "
                     "evicted %.1f, drops for room %lu, key mismatches %.1f, refreshed %.1f (%.0f vertices), primitives left out %.1f, pass draws %.1f, flushes queued %.1f; GX FIFO writes %.0f per frame; textures %zu, %lu invalidated\n", refusals.c_str(),
                     perf.meshInvalidated / n, perf.meshOverflows / n, perf.meshBudgetDrops / n, perf.meshesEvicted / n, static_cast<unsigned long>(perf.meshesDroppedForRoom), perf.meshMismatches / n, perf.meshesRefreshed / n,
                     perf.meshVerticesRefreshed / n, perf.leftOutDraws / n, perf.passDraws / n, perf.flushesQueued / n,
                     (smgFifoWrites - perf.writesBefore) / n, textures.size(), static_cast<unsigned long>(perf.texturesInvalidated));
        if (perf.meshMismatches)
            std::fprintf(stderr, "gx perf: key mismatches by what changed (per frame): key %.1f, descriptor %.1f, format table %.1f, array base %.1f, "
                         "stride %.1f\n", perf.mismatchWhy[0] / n, perf.mismatchWhy[1] / n, perf.mismatchWhy[2] / n, perf.mismatchWhy[3] / n,
                         perf.mismatchWhy[4] / n);
        u32 zoneSamples[ZoneCount], zoneTotal = 0;
        for (unsigned z = 0; z < ZoneCount; ++z) zoneTotal += zoneSamples[z] = sZoneSamples[z].exchange(0, std::memory_order_relaxed);
        if (zoneTotal) {
            // Each zone's share of the window, as milliseconds of an average frame.
            std::string zones;
            for (unsigned z = 0; z < ZoneCount; ++z) {
                if (!zoneSamples[z]) continue;
                char item[40];
                std::snprintf(item, sizeof item, "%s %s %.2f", zones.empty() ? "" : ",", gxZoneName(z), ms / n * zoneSamples[z] / zoneTotal);
                zones += item;
            }
            std::fprintf(stderr, "gx perf: render thread zones, ms per frame (%lu samples):%s\n", static_cast<unsigned long>(zoneTotal), zones.c_str());
        }
        unsigned long long gameWaits = 0, otherWaits = 0;
        smgCpuWaits(&gameWaits, &otherWaits);
        std::fprintf(stderr, "gx perf: waits to turn interrupts off, per frame: the game's thread %.2f ms, the others %.2f ms\n",
                     gameWaits * 1000.0 / SYSCLOCK_ARM11 / n, otherWaits * 1000.0 / SYSCLOCK_ARM11 / n);
        perf = Perf{};
        perf.since = now, perf.listsBefore = lastGpu->displayListCalls, perf.listBytesBefore = lastGpu->displayListBytes;
    }

    void dropAllTextures() {
        dropAtlases();
        if (inFrame) pause(), resume();
        for (auto& [key, texture] : textures) C3D_TexDelete(&texture);
        perf.texturesInvalidated += textures.size();
        textures.clear(), texturePages.clear();
        stateValid = applied = false;
        forgetSent();
    }

    // Textures made from memory a copy has just written are stale.
    void forget(u32 address, std::size_t size) {
        for (u32 member : atlasAddresses)
            if (member >= address && member < address + size) {
                dropAtlases();
                break;
            }
        for (auto it = textures.begin(); it != textures.end();) {
            if (std::get<0>(it->first) >= address && std::get<0>(it->first) < address + size) {
                C3D_TexDelete(&it->second);
                it = textures.erase(it);
            } else {
                ++it;
            }
        }
    }

    // The EFB is the 400x240 target, so each EFB pixel is read from the target pixel it was drawn to, with the precision
    // of the EFB's pixel format (PE_CONTROL): RGB8 has no alpha, RGBA6 six bits a channel, RGB565 its own.
    struct Efb {
        const u32* pixels;
        float toScreenX, toScreenY;
        u32 format;
    };
    static std::array<std::uint8_t, 4> efbPixel(const void* context, int x, int y) {
        const Efb& e = *static_cast<const Efb*>(context);
        const int sx = std::clamp(static_cast<int>((static_cast<float>(x) + 0.5f) * e.toScreenX), 0, 399);
        const int sy = std::clamp(static_cast<int>((static_cast<float>(y) + 0.5f) * e.toScreenY), 0, 239);
        const u32 v = e.pixels[sx * 240 + (239 - sy)];  // the target is the screen turned a quarter, as the LCD is
        std::array<std::uint8_t, 4> c{std::uint8_t(v >> 24), std::uint8_t(v >> 16), std::uint8_t(v >> 8), std::uint8_t(v)};
        const auto keep = [](std::uint8_t value, int bits) { return std::uint8_t((value >> (8 - bits) << (8 - bits)) | value >> bits); };
        if (e.format == 0) c[3] = 255;
        if (e.format == 1) for (auto& channel : c) channel = keep(channel, 6);
        if (e.format == 2) c[0] = keep(c[0], 5), c[1] = keep(c[1], 6), c[2] = keep(c[2], 5), c[3] = 255;
        return c;
    }

    // A depth copy's pixel: z's bytes, high first, as GX keeps z (the port keeps nearer as larger).
    static std::array<std::uint8_t, 4> efbDepth(const void* context, int x, int y) {
        const Efb& e = *static_cast<const Efb*>(context);
        const int sx = std::clamp(static_cast<int>((static_cast<float>(x) + 0.5f) * e.toScreenX), 0, 399);
        const int sy = std::clamp(static_cast<int>((static_cast<float>(y) + 0.5f) * e.toScreenY), 0, 239);
        const u32 z = 0xFFFFFF - (e.pixels[sx * 240 + (239 - sy)] & 0xFFFFFF);
        return {std::uint8_t(z >> 16), std::uint8_t(z >> 8), std::uint8_t(z), 255};
    }

    void textureCopy(const GxCommandProcessor& gpu, std::uint32_t command) {
        static const bool null = smgDevGxNull();
        if (null) return;
        GxTextureCopy copy;
        if (const char* why = readTextureCopy(gpu, command, copy)) stop(why);
        if (dumping)
            std::fprintf(stderr, "gx dump: copy (%u,%u %ux%u) to 0x%08lx format %u%s%s%s\n", copy.x, copy.y, copy.width, copy.height,
                         static_cast<unsigned long>(copy.address), copy.format, copy.intensity ? " intensity" : "", copy.halfScale ? " half" : "",
                         copy.clear ? " clear" : "");
        if (dumping) flushBatch(), dumpEfbAverage("at the copy");
        if (!inFrame) begin();
        flushBatch();
        if (sCopiesStayOnGpu && gpuCopy(gpu, copy)) {
            ++perf.gpuCopies;
        } else {
            ++perf.cpuCopies;
            const u64 start = svcGetSystemTick();
            dropGpuCopy(copy.address);
            const Efb efb = copy.depth ? Efb{readEfb(true), 400.0f / frame.width, 240.0f / frame.height, 3} : efbView(gpu);
            encodeTextureCopy(copy, copy.depth ? efbDepth : efbPixel, &efb, static_cast<std::uint8_t*>(OSPhysicalToCached(copy.address)));
            perf.cpuCopyTicks += svcGetSystemTick() - start;
            static unsigned noted = 0;
            if (dumping || noted++ < 4)
                std::fprintf(stderr, "gx perf: CPU %scopy (%u,%u %ux%u) to 0x%08lx format %u took %.2f ms\n", copy.depth ? "depth " : "", copy.x, copy.y,
                             copy.width, copy.height, static_cast<unsigned long>(copy.address), copy.format,
                             double(svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11);
        }
        forget(copy.address, textureCopySize(copy));
        stateValid = applied = false;  // its textures may be gone
        forgetSent();
        if (copy.clear) C3D_RenderTargetClear(target, C3D_CLEAR_ALL, copyClearColor(gpu), copyClearDepth(gpu)), fresh[0] = fresh[1] = fresh[2] = false;
    }

    Efb efbView(const GxCommandProcessor& gpu) {
        return {readEfb(false), 400.0f / frame.width, 240.0f / frame.height, PE_CONTROL_GET_PIXTYPE(gpu.bp[0x43])};
    }

    void dropGpuCopy(u32 address) {
        const auto found = gpuCopies.find(address);
        if (found == gpuCopies.end()) return;
        C3D_RenderTargetDelete(found->second.target);
        C3D_TexDelete(&found->second.texture);
        gpuCopies.erase(found);
    }

    // An EFB copy the game makes for its own GPU textures (JUTTexture::capture) stays in the PICA's memory: the target is
    // copied whole into a texture, which a quad then draws into the copy's texture through the combiner, with the copy's
    // vertical filter and its format's channels. The Wii's memory is not written, so only the GPU sees this copy.
    bool gpuCopy(const GxCommandProcessor& gpu, const GxTextureCopy& copy) {
        if (copy.format == 11 || copy.format == 12) return false;  // two channels a texel: the CPU's encoder packs them
        // A depth copy to one of z's bytes: the depth target read as RGBA8. Halved, the bilinear sample at each 2x2 block's
        // center averages the byte alone, within one of z's own average's.
        if (copy.depth && copy.format != 1 && copy.format != 8 && copy.format != 9 && copy.format != 10) return false;
        int rows[3] = {};
        std::uint8_t weights[3] = {};
        unsigned taps = copy.depth ? 1 : 0;
        for (int i = 0; i < 7 && !copy.depth; ++i) {
            if (!copy.filter[i]) continue;
            if (taps == 3) return false;  // the PICA samples three textures at once
            rows[taps] = i - 3, weights[taps++] = copy.filter[i];
        }
        const bool noAlpha = PE_CONTROL_GET_PIXTYPE(gpu.bp[0x43]) == 0;
        if (!taps || (!copy.depth && copyStages(copy, taps, noAlpha) > CombinerProgram::MaxStages)) return false;
        const unsigned width = textureSide(std::min(float(copy.texWidth), copy.width * 400.0f / frame.width));
        const unsigned height = textureSide(std::min(float(copy.texHeight), copy.height * 240.0f / frame.height));
        GpuCopy& g = gpuCopies[copy.address];
        if (g.target && (g.texture.width != width || g.texture.height != height)) {
            C3D_RenderTargetDelete(g.target);
            C3D_TexDelete(&g.texture);
            g.target = nullptr;
        }
        if (!g.target) {
            // Render targets live in VRAM; with none left, the copy is the CPU's after all.
            if (!C3D_TexInitVRAM(&g.texture, width, height, GPU_RGBA8)) return gpuCopies.erase(copy.address), false;
            g.target = C3D_RenderTargetCreateFromTex(&g.texture, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
            if (!g.target) stop("an EFB copy's texture as a render target");
        }
        g.width = copy.texWidth, g.height = copy.texHeight, g.readFormat = copyReadFormat(copy.format);
        if (!scratchReady) {
            if (!C3D_TexInitVRAM(&scratch, 256, 512, GPU_RGBA8) && !withRoom([&] { return C3D_TexInit(&scratch, 256, 512, GPU_RGBA8); }))
                stop("an EFB copy with no memory to take the target into");
            C3D_TexSetFilter(&scratch, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&scratch, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            scratchReady = true;
        }
        roomInQueue(2);
        roomFor(18);
        // Queued behind the draws so far, as the pass is behind it. Split here with the flush: C3D_SyncTextureCopy's split
        // leaves its part of the command list in the CPU's cache, and a real GPU then read stale words and froze.
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
        // Only the target's tile rows (8 screen columns each) the copy samples, a column either side for the filter.
        const float left = float(copy.x) * 400.0f / frame.width, right = float(copy.x + copy.width) * 400.0f / frame.width;
        // Never none: a transfer of no bytes freezes a real 3DS.
        const unsigned firstRow = std::min(49u, unsigned(std::clamp(left - 1.0f, 0.0f, 400.0f)) / 8);
        const unsigned endRow = std::max(firstRow + 1, std::min(50u, (unsigned(std::clamp(right + 1.0f, 0.0f, 400.0f)) + 7) / 8));
        u8* const from = static_cast<u8*>(copy.depth ? target->frameBuf.depthBuf : target->frameBuf.colorBuf) + firstRow * TargetTileRow;
        GX_TextureCopy(reinterpret_cast<u32*>(from), GX_BUFFER_DIM(TargetTileRow / 16, 0),
                       reinterpret_cast<u32*>(static_cast<u8*>(scratch.data) + firstRow * ScratchTileRow),
                       GX_BUFFER_DIM(TargetTileRow / 16, (ScratchTileRow - TargetTileRow) / 16), TargetTileRow * (endRow - firstRow),
                       GX_TRANSFER_RAW_COPY(1));

        // Three bands, so the taps that reach past the copy's first and last rows can be clamped as the PE clamps them.
        const float step = copy.halfScale ? 2.0f : 1.0f, edge = 2.0f / float(copy.texHeight);
        const float bands[4] = {0, std::min(edge, 0.5f), std::max(1 - edge, 0.5f), 1};
        const float top = copy.clampTop ? float(copy.y) : 0, bottom = copy.clampBottom ? float(copy.y + copy.height) : frame.height;
        GxDrawVertex* out = vertices + used;
        const auto corner = [&](float u, float v) {
            GxDrawVertex vertex{};
            vertex.position[0] = 2 * u - 1, vertex.position[1] = 2 * v - 1, vertex.position[2] = -0.5f, vertex.position[3] = 1;
            for (unsigned tap = 0; tap < taps; ++tap) {
                const float ex = float(copy.x) + u * float(copy.width);
                const float ey = std::clamp(float(copy.y) + v * float(copy.height) + float(rows[tap]) * step, top, bottom);
                // The EFB pixel on the screen, then where the target holds it: the screen turned a quarter, as the LCD is.
                const float sx = std::clamp(ex * 400.0f / frame.width, 0.5f, 399.5f), sy = std::clamp(ey * 240.0f / frame.height, 0.5f, 239.5f);
                vertex.texcoord[tap][0] = (240.0f - sy) / 256.0f, vertex.texcoord[tap][1] = (512.0f - sx) / 512.0f;
            }
            return vertex;
        };
        unsigned n = 0;
        for (int band = 0; band < 3; ++band) {
            const GxDrawVertex a = corner(0, bands[band]), b = corner(1, bands[band]), c = corner(1, bands[band + 1]), d = corner(0, bands[band + 1]);
            for (const GxDrawVertex& v : {a, b, c, a, c, d}) out[n++] = v;
        }

        C3D_FrameDrawOn(g.target);
        if (copy.depth) depthCombiner(copy.format), C3D_TexSetFilter(&scratch, copy.halfScale ? GPU_LINEAR : GPU_NEAREST, GPU_NEAREST);
        else copyCombiner(copy, taps, weights, noAlpha);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        for (int unit = 0; unit < 3; ++unit) C3D_TexBind(unit, &scratch);
        unitsBound = 7;
        forgetSent();
        fogOff();
        ensureCpuBuffers();
        C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(used), static_cast<int>(n));
        used += n;
        applied = false;
        if (copy.depth) C3D_TexSetFilter(&scratch, GPU_LINEAR, GPU_LINEAR);  // the draw has taken the nearest already
        C3D_FrameDrawOn(target);
        noteGpuCopy(gpu, copy);
        return true;
    }

    // z's byte from a D24S8 word read as RGBA8 (z's top byte in G, then B, then A), as GX keeps z: 1 less the port's
    // nearer-is-larger, byte for byte.
    void depthCombiner(std::uint8_t format) {
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0);
        C3D_TexEnvOpRgb(env, format == 9 ? GPU_TEVOP_RGB_ONE_MINUS_SRC_B : format == 10 ? GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_RGB_ONE_MINUS_SRC_G);
        C3D_TexEnvOpAlpha(env, format == 9 ? GPU_TEVOP_A_ONE_MINUS_SRC_B : format == 10 ? GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA : GPU_TEVOP_A_ONE_MINUS_SRC_G);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        for (int stage = 1; stage < int(CombinerProgram::MaxStages); ++stage) C3D_TexEnvInit(C3D_GetTexEnv(stage));
        C3D_TexEnvBufUpdate(C3D_Both, 0);
    }

    // Once per format and size, the GPU's copy is read back and set against encodeTextureCopy's from the same target,
    // as drawn and mirrored each way: the log shows the PICA's orientation and channels agree (or how they don't).
    void noteGpuCopy(const GxCommandProcessor& gpu, const GxTextureCopy& copy) {
        const u64 kind = u64(copy.format) | u64(copy.intensity) << 4 | u64(copy.halfScale) << 5 | u64(copy.depth) << 6 | u64(copy.texWidth) << 8 |
                         u64(copy.texHeight) << 24;
        if (std::find(checkedKinds.begin(), checkedKinds.end(), kind) != checkedKinds.end()) return;
        checkedKinds.push_back(kind);
        pause(), resume();  // the GPU has drawn the copy's quad
        const Efb efb = copy.depth ? Efb{readEfb(true), 400.0f / frame.width, 240.0f / frame.height, 3} : efbView(gpu);
        std::vector<std::uint8_t> bytes(textureCopySize(copy));
        encodeTextureCopy(copy, copy.depth ? efbDepth : efbPixel, &efb, bytes.data());
        Image expected;
        std::string error;
        if (!decodeGxImage({bytes.data(), bytes.size()}, static_cast<GxFormat>(copyReadFormat(copy.format)), copy.texWidth, copy.texHeight, {},
                           GxPalette::IA8, expected, error))
            return void(std::fprintf(stderr, "The Wii GPU's drawing: checking a GPU copy: %s\n", error.c_str()));
        const C3D_Tex& texture = gpuCopies[copy.address].texture;
        GSPGPU_InvalidateDataCache(texture.data, texture.size);
        const auto* texels = static_cast<const u32*>(texture.data);
        const unsigned w = texture.width, h = texture.height;
        const auto morton = [](unsigned x, unsigned y) {
            return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3;
        };
        double differ[4] = {};
        constexpr int Grid = 24;
        for (int gy = 0; gy < Grid; ++gy) {
            for (int gx = 0; gx < Grid; ++gx) {
                const float u = (gx + 0.5f) / Grid, v = (gy + 0.5f) / Grid;
                const u32 want = expected.pixels[unsigned(v * copy.texHeight) * copy.texWidth + unsigned(u * copy.texWidth)];
                for (int mirror = 0; mirror < 4; ++mirror) {
                    const unsigned x = unsigned((mirror & 1 ? 1 - u : u) * w), row = unsigned((mirror & 2 ? v : 1 - v) * h);
                    const u32 got = texels[((row / 8) * (w / 8) + x / 8) * 64 + morton(x & 7, row & 7)];
                    for (int shift = 0; shift < 32; shift += 8) differ[mirror] += std::abs(int(want >> shift & 255) - int(got >> shift & 255));
                }
            }
        }
        for (double& d : differ) d /= Grid * Grid * 4;
        std::fprintf(stderr, "The Wii GPU's drawing: GPU EFB copy (format %u%s%s, %ux%u in %ux%u) differs from the CPU's by %.1f as drawn, "
                     "%.1f mirrored across, %.1f upside down, %.1f both\n", copy.format, copy.intensity ? " intensity" : "", copy.halfScale ? " half" : "",
                     copy.texWidth, copy.texHeight, w, h, differ[0], differ[1], differ[2], differ[3]);
    }

    // Combiner stages a copy needs: its taps, alpha made one for an EFB without it, then what its format keeps.
    static unsigned copyStages(const GxTextureCopy& copy, unsigned taps, bool noAlpha) {
        const std::uint8_t f = copy.format;
        const bool usesIntensity = f <= 3 || f == 8;
        return taps + noAlpha + (usesIntensity && copy.intensity ? 3 : f <= 3 || f >= 7 ? 1 : 0) + (f == 4 && !noAlpha);
    }

    // The taps summed in 64ths, then the format as encodeTextureCopy writes it: the PE's intensity (studio-range Y, by a
    // dot product) or one channel for every component, or color; RGB565 reads back with alpha one.
    void copyCombiner(const GxTextureCopy& copy, unsigned taps, const std::uint8_t* weights, bool noAlpha) {
        const GPU_TEVSRC units[3] = {GPU_TEXTURE0, GPU_TEXTURE1, GPU_TEXTURE2};
        const auto byte = [](float value) { return u32(std::lround(std::clamp(value, 0.0f, 1.0f) * 255)); };
        const auto gray = [&](float value) { const u32 b = byte(value); return b | b << 8 | b << 16 | b << 24; };
        unsigned stage = 0;
        for (; stage < taps; ++stage) {
            C3D_TexEnv* env = C3D_GetTexEnv(int(stage));
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, units[stage], GPU_CONSTANT, stage ? GPU_PREVIOUS : GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, stage ? GPU_MULTIPLY_ADD : GPU_MODULATE);
            C3D_TexEnvColor(env, gray(weights[stage] / 64.0f));
        }
        const auto next = [&] {
            C3D_TexEnv* env = C3D_GetTexEnv(int(stage++));
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
            return env;
        };
        const auto opaque = [&] {
            C3D_TexEnv* env = next();
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT);
            C3D_TexEnvColor(env, 0xFF000000);
        };
        if (noAlpha) opaque();
        const std::uint8_t f = copy.format;
        const bool usesIntensity = f <= 3 || f == 8, pairs = f == 2 || f == 3;
        if (usesIntensity && copy.intensity) {
            // DOT3 gives 4 * sum((a - 1/2)(b - 1/2)): with a = c/2 + 1/2 and b = w/2 + 1/2 that is sum(c * w).
            C3D_TexEnv* half = next();
            C3D_TexEnvFunc(half, C3D_RGB, GPU_MULTIPLY_ADD);
            C3D_TexEnvColor(half, gray(0.5f));
            C3D_TexEnv* dot = next();
            C3D_TexEnvFunc(dot, C3D_RGB, GPU_DOT3_RGB);
            C3D_TexEnvColor(dot, byte(0.5f + 66 / 512.0f) | byte(0.5f + 129 / 512.0f) << 8 | byte(0.5f + 25 / 512.0f) << 16);
            C3D_TexEnv* bias = next();
            C3D_TexEnvFunc(bias, C3D_Both, GPU_ADD);
            C3D_TexEnvColor(bias, pairs ? gray(16 / 255.0f) & 0x00FFFFFF : gray(16 / 255.0f));
            if (!pairs) C3D_TexEnvOpAlpha(bias, GPU_TEVOP_A_SRC_R);
        } else if (usesIntensity || f >= 7) {
            const GPU_TEVOP_RGB rgb = f == 7 ? GPU_TEVOP_RGB_SRC_ALPHA : f == 9 ? GPU_TEVOP_RGB_SRC_G : f == 10 ? GPU_TEVOP_RGB_SRC_B : GPU_TEVOP_RGB_SRC_R;
            const GPU_TEVOP_A alpha = f == 7 || pairs ? GPU_TEVOP_A_SRC_ALPHA : f == 9 ? GPU_TEVOP_A_SRC_G : f == 10 ? GPU_TEVOP_A_SRC_B : GPU_TEVOP_A_SRC_R;
            C3D_TexEnv* pick = next();
            C3D_TexEnvOpRgb(pick, rgb);
            C3D_TexEnvOpAlpha(pick, alpha);
        }
        if (f == 4 && !noAlpha) opaque();
        for (; stage < CombinerProgram::MaxStages; ++stage) C3D_TexEnvInit(C3D_GetTexEnv(int(stage)));
        C3D_TexEnvBufUpdate(C3D_Both, 0);
    }

    // Y2R, the 3DS's YUV converter, writes the frame into the texture's 8x8 tiles itself, a row of tiles per transfer.
    bool convertByY2r(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, C3D_Tex& texture) {
        if (!y2rTried) {
            y2rTried = true;
            y2rOpen = R_SUCCEEDED(y2rInit()) && R_SUCCEEDED(Y2RU_SetTransferEndInterrupt(true)) && R_SUCCEEDED(Y2RU_GetTransferEndEvent(&y2rDone));
            if (!y2rOpen) std::fprintf(stderr, "The Wii's THP movies: Y2R won't open, so their frames turn to RGB on the CPU\n");
        }
        if (!y2rOpen || width % 8 || height % 8 || width > texture.width) return false;
        Y2RU_ConversionParams params{};
        params.input_format = INPUT_YUV420_INDIV_8, params.output_format = OUTPUT_RGB_16_565, params.rotation = ROTATION_NONE;
        params.block_alignment = BLOCK_8_BY_8, params.input_line_width = static_cast<s16>(width), params.input_lines = static_cast<s16>(height);
        params.standard_coefficient = COEFFICIENT_ITU_R_BT_601, params.alpha = 0xFF;
        const u32 luma = width * height;
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(y), luma);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(u), luma / 4);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, reinterpret_cast<u32>(v), luma / 4);
        GSPGPU_FlushDataCache(texture.data, texture.size);  // no dirty line left to land over what Y2R writes
        svcClearEvent(y2rDone);
        const s16 tileRow = static_cast<s16>(width * 8 * 2), gap = static_cast<s16>((texture.width - width) * 8 * 2);
        if (R_FAILED(Y2RU_SetConversionParams(&params)) || R_FAILED(Y2RU_SetSendingY(y, luma, static_cast<s16>(width), 0)) ||
            R_FAILED(Y2RU_SetSendingU(u, luma / 4, static_cast<s16>(width / 2), 0)) || R_FAILED(Y2RU_SetSendingV(v, luma / 4, static_cast<s16>(width / 2), 0)) ||
            R_FAILED(Y2RU_SetReceiving(texture.data, luma * 2, tileRow, gap)) || R_FAILED(Y2RU_StartConversion()))
            stop("Y2R refusing a movie frame");
        // Its end event is sometimes never signaled (libctru's note), so a timeout falls back to asking.
        if (R_FAILED(svcWaitSynchronization(y2rDone, 100'000'000)))
            for (bool busy = true; R_SUCCEEDED(Y2RU_IsBusyConversion(&busy)) && busy;) svcSleepThread(1'000'000);
        return true;
    }

    // THPGXYuv2RgbDraw: a decoded frame's planes (smg::decodeThpVideo) turned to RGB once, drawn over the EFB rectangle the
    // game's quad covers, its color replacing what is there (no blending, depth or alpha written).
    void movieFrame(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, unsigned generation,
                    int x, int top, int w, int h) {
        MovieFrame& m = movie[generation & 1];
        const unsigned tw = textureSide(float(width)), th = textureSide(float(height));
        if (m.ready && (m.texture.width != tw || m.texture.height != th)) C3D_TexDelete(&m.texture), m.ready = false;
        if (!m.ready) {
            if (!withRoom([&] { return C3D_TexInit(&m.texture, tw, th, GPU_RGB565); })) stop("a movie frame with no memory for its texture");
            C3D_TexSetFilter(&m.texture, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&m.texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            m.ready = true, m.key = nullptr;
        }
        if (!inFrame) begin();
        flushBatch();
        if (m.key != y || m.generation != generation) {
            m.topFirst = convertByY2r(y, u, v, width, height, m.texture);
            if (!m.topFirst) thpTiles565(y, u, v, width, height, static_cast<std::uint16_t*>(m.texture.data), tw, th), C3D_TexFlush(&m.texture);
            m.key = y, m.generation = generation;
        }
        roomFor(6);
        GxDrawVertex* out = vertices + used;
        const float s1 = float(width) / float(tw), t1 = float(height) / float(th);
        const auto corner = [&](float ex, float ey, float s, float t) {
            GxDrawVertex vertex{};
            const float cx = 2 * ex / frame.width - 1, cy = 1 - 2 * ey / frame.height;
            vertex.position[0] = cy, vertex.position[1] = -cx, vertex.position[2] = -0.5f, vertex.position[3] = 1;
            vertex.texcoord[0][0] = s, vertex.texcoord[0][1] = t;
            return vertex;
        };
        const float tTop = m.topFirst ? 1 : 0, tBottom = m.topFirst ? 1 - t1 : t1;
        const GxDrawVertex a = corner(x, top, 0, tTop), b = corner(x + w, top, s1, tTop), c = corner(x + w, top + h, s1, tBottom), d = corner(x, top + h, 0, tBottom);
        unsigned n = 0;
        for (const GxDrawVertex& vertex : {a, b, c, a, c, d}) out[n++] = vertex;
        for (int stage = 0; stage < int(CombinerProgram::MaxStages); ++stage) C3D_TexEnvInit(C3D_GetTexEnv(stage));
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        C3D_TexEnvBufUpdate(C3D_Both, 0);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_DepthTest(false, GPU_ALWAYS, static_cast<GPU_WRITEMASK>(GPU_WRITE_COLOR & ~GPU_WRITE_ALPHA));
        C3D_CullFace(GPU_CULL_NONE);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        C3D_TexBind(0, &m.texture);
        unitsBound |= 1;
        forgetSent();
        fogOff();
        ensureCpuBuffers();
        C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(used), static_cast<int>(n));
        used += n;
        applied = false;
        fresh[0] = fresh[1] = fresh[2] = false;
    }

    // What has been drawn so far, read back from the target once the GPU has finished it; depth (D24S8) moves as raw
    // four-byte pixels too.
    const u32* readEfb(bool depth) {
        u32*& into = readback[depth];
        if (!into) into = static_cast<u32*>(linearAlloc(240 * 400 * 4));
        if (!into) stop("reading the EFB back with no memory to read it into");
        if (!fresh[depth]) {
            if (inFrame) pause();
            void* buffer = depth ? target->frameBuf.depthBuf : target->frameBuf.colorBuf;
            C3D_SyncDisplayTransfer(static_cast<u32*>(buffer), GX_BUFFER_DIM(240, 400), into, GX_BUFFER_DIM(240, 400), ReadbackFlags);
            GSPGPU_InvalidateDataCache(into, 240 * 400 * 4);
            if (inFrame) resume();
            fresh[depth] = true;
        }
        return into;
    }

    std::uint32_t peek(const GxCommandProcessor& gpu, unsigned x, unsigned y, bool depth) {
        if (!ready) return depth ? 0xFFFFFF : 0;
        if (!depth) {
            const Efb efb{readEfb(false), 400.0f / frame.width, 240.0f / frame.height, PE_CONTROL_GET_PIXTYPE(gpu.bp[0x43])};
            const auto c = efbPixel(&efb, int(x), int(y));
            return u32(c[3]) << 24 | u32(c[0]) << 16 | u32(c[1]) << 8 | c[2];
        }
        const int sx = std::clamp(static_cast<int>((static_cast<float>(x) + 0.5f) * 400.0f / frame.width), 0, 399);
        const int sy = std::clamp(static_cast<int>((static_cast<float>(y) + 0.5f) * 240.0f / frame.height), 0, 239);
        return 0xFFFFFF - (depthFromTile(unsigned(sx), unsigned(239 - sy)) & 0xFFFFFF);  // the port keeps nearer as larger
    }

    // One depth value from the target's 8x8 tile that holds it (row `row` of 400, column `column` of 240, as the
    // whole readback lays them out): a 256-byte copy instead of the whole buffer.
    u32* depthTile = nullptr;
    u32 depthFromTile(unsigned row, unsigned column) {
        const u64 start = svcGetSystemTick();
        ++perf.peeks;
        if (!depthTile) depthTile = static_cast<u32*>(linearAlloc(256));
        if (!depthTile) stop("reading a depth tile back with no memory to read it into");
        const unsigned tile = (row / 8) * 30 + column / 8;
        if (!inFrame)
            for (WantedTile& wanted : peeksBetweenFrames)
                if (wanted.tile == tile || wanted.tile == ~0u || framesShown - wanted.asked > 3) {
                    wanted = {tile, framesShown};
                    break;
                }
        if (fresh[2] && tile == freshTile) return depthTile[morton(column & 7, row & 7)];  // nothing drawn since
        // The tile as the GPU copied it up to three frames before, or the nearest copied texel when the peek moved (the
        // star pointer, a balloon's anchor), from either thread: waiting for the GPU instead costs a real 3DS ~40 ms.
        const PeekTile *done = nullptr, *near = nullptr;
        unsigned nearDistance = 4;  // in tiles; a peek that far from every copied tile waits
        bool queued = false;
        for (const PeekTile& p : peekTiles) {
            const bool copied = p.begun < framesBegun && framesShown - p.frame <= 3;
            if (p.tile != tile) {
                const unsigned distance = std::max(std::max(p.tile / 30, tile / 30) - std::min(p.tile / 30, tile / 30),
                                                   std::max(p.tile % 30, tile % 30) - std::min(p.tile % 30, tile % 30));
                if (copied && (distance < nearDistance || (distance == nearDistance && near && p.begun > near->begun))) near = &p, nearDistance = distance;
                continue;
            }
            if (copied && (!done || p.begun > done->begun)) done = &p;
            queued |= p.begun == framesBegun;
        }
        if (inFrame && !queued) queuePeekTile(tile);
        if (!done && near) {
            done = near;
            row = std::clamp(row, near->tile / 30 * 8, near->tile / 30 * 8 + 7);
            column = std::clamp(column, near->tile % 30 * 8, near->tile % 30 * 8 + 7);
        }
        if (done) {
            GSPGPU_InvalidateDataCache(done->texels, 256);
            ++perf.peeksDeferred;
            return done->texels[morton(column & 7, row & 7)];
        }
        if (inFrame) pause();
        C3D_SyncTextureCopy(static_cast<u32*>(target->frameBuf.depthBuf) + tile * 64, GX_BUFFER_DIM(256 / 16, 0), depthTile, GX_BUFFER_DIM(256 / 16, 0), 256,
                            GX_TRANSFER_RAW_COPY(1));
        GSPGPU_InvalidateDataCache(depthTile, 256);
        if (inFrame) resume();
        perf.peekTicks += svcGetSystemTick() - start;
        fresh[2] = true, freshTile = tile;
        return depthTile[morton(column & 7, row & 7)];
    }

    // A depth peek (the star pointer's, a talking NPC's: both from draw-sync callbacks, their answer used the frame after)
    // is answered from its tile as the GPU copied it at a peek the frame before, so the frame isn't stopped for it: each
    // peek queues that copy behind what was drawn so far. A copy is done once a later C3D_FrameBegin has waited for it.
    struct PeekTile {
        u32* texels = nullptr;
        unsigned tile = ~0u;
        u32 begun = 0, frame = 0;  // framesBegun and framesShown when it was queued
    };
    static constexpr unsigned PeekTiles = 16;
    PeekTile peekTiles[PeekTiles];
    unsigned nextPeekTile = 0;
    // Tiles peeks between frames read, copied at each frame's end (where they read) while they keep asking.
    struct WantedTile {
        unsigned tile = ~0u;
        u32 asked = 0;  // framesShown then
    };
    WantedTile peeksBetweenFrames[2];
    u32 framesBegun = 0;
    void queuePeekTile(unsigned tile) {
        PeekTile& p = peekTiles[nextPeekTile++ % PeekTiles];
        if (!p.texels) p.texels = static_cast<u32*>(linearAlloc(256));
        if (!p.texels) stop("copying a depth tile with no memory to copy it into");
        roomInQueue(2);
        flushBatch();
        flushVertices();
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
        GX_TextureCopy(static_cast<u32*>(target->frameBuf.depthBuf) + tile * 64, GX_BUFFER_DIM(256 / 16, 0), p.texels, GX_BUFFER_DIM(256 / 16, 0), 256,
                       GX_TRANSFER_RAW_COPY(1));
        p.tile = tile, p.begun = framesBegun, p.frame = framesShown;
    }

    static u32 copyClearColor(const GxCommandProcessor& gpu) {
        const std::uint32_t ar = gpu.bp[0x4F], gb = gpu.bp[0x50];
        // citro3d's clear color is 0xRRGGBBAA.
        return (ar & 0xFF) << 24 | (gb >> 8 & 0xFF) << 16 | (gb & 0xFF) << 8 | (ar >> 8 & 0xFF);
    }
    // Its depth buffer keeps nearer as larger.
    static u32 copyClearDepth(const GxCommandProcessor& gpu) { return 0xFFFFFF - (gpu.bp[0x51] & 0xFFFFFF); }

    // PE_COPY_CMD: opcode 1 copies to the display, 0 to a texture; bit 11 clears the EFB after.
    void copy(const GxCommandProcessor& gpu, std::uint32_t command) {
        lastGpu = &gpu;
        if (!(command >> 14 & 1)) return textureCopy(gpu, command);
        const std::uint32_t size = gpu.bp[0x4A];
        frame.width = static_cast<float>((size & 0x3FF) + 1), frame.height = static_cast<float>((size >> 10 & 0x3FF) + 1);
        if (!inFrame) begin();
        for (WantedTile& wanted : peeksBetweenFrames) {
            if (wanted.tile == ~0u) continue;
            if (framesShown - wanted.asked > 3) wanted.tile = ~0u;
            else if (std::none_of(std::begin(peekTiles), std::end(peekTiles), [&](const PeekTile& p) { return p.tile == wanted.tile && p.begun == framesBegun; }))
                queuePeekTile(wanted.tile);
        }
        flushBatch();
        if (dumping) std::fprintf(stderr, "gx dump: frame ends after %u draws\n", dumpIndex), dumping = false;
        flushVertices();
        fogOff();
        C3D_FrameEnd(GX_CMDLIST_FLUSH);
        inFrame = false;
        if (smgAvSyncFramePending.exchange(false))
            std::fprintf(stderr, "av sync: the marked frame goes out %.1f ms after the mark\n",
                         double(svcGetSystemTick() - smgAvSyncMarkTick()) * 1000.0 / SYSCLOCK_ARM11);
        stateValid = false;  // read again each frame, as a Z texture the CPU rewrites would be
        if (command >> 11 & 1) clearColor = copyClearColor(gpu), clearDepth = copyClearDepth(gpu), clearPending = true;
    }
};

Renderer& renderer() {
    static Renderer instance;
    return instance;
}
} // namespace

namespace {
// dev.gx_timing: the clock around a renderer entry point, into perf.timed[part].
struct Timed {
    explicit Timed(int part) : part_(part), start_(on() ? svcGetSystemTick() : 0) {}
    ~Timed() {
        if (start_) renderer().perf.timed[part_] += svcGetSystemTick() - start_;
    }
    static bool on() {
        static const bool timing = smgDevGxTiming();
        return timing;
    }
    int part_;
    u64 start_;
};
} // namespace

void gxRenderPrimitive(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices) {
    static const bool null = smgDevGxNull();
    if (null) return;
    const PortMemory memory;
    const GxZoneScope zone(ZonePrimitive);
    const Timed timed(gpu.displayListDepth ? 4 : 0);
    if (gpu.displayListDepth) renderer().perf.listVertices += count, renderer().perf.refusedVertices[renderer().listRefusal] += count;
    renderer().draw(gpu, kind, format, count, vertices);
}

void gxRenderCopiesStayOnGpu(bool stay) {
    smgGXFifo_u8(0x01);  // the port's own command, read in order with the copy it wraps
    smgGXFifo_u8(stay);
}

void gxRenderKeepCopiesOnGpu(bool stay) { sCopiesStayOnGpu = stay; }

bool gxRenderDisplayList(const GxCommandProcessor& gpu, std::uint32_t address, std::uint32_t size) {
    static const bool null = smgDevGxNull();
    if (null) return true;
    const PortMemory memory;
    const Timed timed(1);
    return renderer().displayList(gpu, address, size);
}

void gxRenderMemoryChanged(const void* start, std::uint32_t size) {
    if (!size) return;
    std::atomic_thread_fence(std::memory_order_seq_cst);  // the game's writes before the page bits are read (see markPages)
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(start) >> 12, last = (reinterpret_cast<std::uintptr_t>(start) + size - 1) >> 12;
    bool meshPage = false;
    for (std::uintptr_t page = first; page <= last && !meshPage; ++page)
        meshPage = sMeshPageBits[page >> 5].load(std::memory_order_relaxed) >> (page & 31) & 1;
    if (!meshPage) return;
    LightLock_Lock(&sChanged.lock);
    if (sChanged.count < 1024) {
        sChanged.start[sChanged.count] = reinterpret_cast<std::uintptr_t>(start);
        sChanged.end[sChanged.count++] = reinterpret_cast<std::uintptr_t>(start) + size;
    } else {
        sChanged.overflowed = true;
    }
    sChanged.pending.store(true, std::memory_order_release);
    LightLock_Unlock(&sChanged.lock);
}

void gxRenderMovieFrame(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, unsigned generation,
                        int x, int top, int w, int h) {
    const PortMemory memory;
    const GxZoneScope zone(ZoneMovie);
    const Timed timed(3);
    renderer().movieFrame(y, u, v, width, height, generation, x, top, w, h);
}

std::uint32_t gxRenderPeek(const GxCommandProcessor& gpu, unsigned x, unsigned y, bool depth) {
    const PortMemory memory;
    const GxZoneScope zone(ZonePeek);
    return renderer().peek(gpu, x, y, depth);
}

void gxRenderCopy(const GxCommandProcessor& gpu, std::uint32_t command) {
    const PortMemory memory;
    {
        const GxZoneScope zone(command >> 14 & 1 ? ZoneFrameEnd : ZoneCopy);
        const Timed timed(command >> 14 & 1 ? 5 : 2);
        renderer().copy(gpu, command);
    }
    if (command >> 14 & 1) renderer().perfLine();
}
} // namespace smg

// GameSystem::frameLoop's phases (adapted): 0 drawing begins, 1 the logic step, 2 animation, 3 waiting for the GPU's
// draw done, 4 the retrace wait. Ticks are read only here, a few times a frame: Azahar charges 150 cycles for each read.
extern "C" void smgFramePhase(int phase) {
    static u64 last = 0;
    static int current = -1;
    static unsigned frames = 0;
    const u64 now = svcGetSystemTick();
    if (current >= 0) smg::sPhaseTicks[current].fetch_add(static_cast<u32>(now - last), std::memory_order_relaxed);
    if (phase == 0 && current >= 0) smg::sPhaseFrames.fetch_add(1, std::memory_order_relaxed);
    current = phase, last = now;
    // Every 300th frame the game's state is logged (source/compat/GameScene.cpp), from the game's own thread.
    if (phase == 0) smgDevWarpStep();
    if (phase == 0 && ++frames % 300 == 0) smgHeartbeat();
}
