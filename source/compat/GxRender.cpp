// The Wii GPU's drawing on the 3DS. Each primitive the command processor hands over becomes triangles transformed on
// the CPU (smg::buildGxDraw) and drawn with citro3d, the TEV program as combiner stages and the textures fetched from
// the Wii's memory. An EFB copy to the display ends the frame. What this can't draw yet stops by name.
#include <3ds.h>
#include <citro3d.h>
#include <GxShader.hpp>
#include <private/pe_reg.h>
#include <smg/FreeStore.hpp>
#include <smg/GxFifoWrite.h>
#include <smg/GxCopy.hpp>
#include <smg/GxDraw.hpp>
#include <smg/GxRender.hpp>
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
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" void smgHeartbeat();          // source/compat/GameScene.cpp
extern "C" unsigned smgMovieDecodes();  // source/compat/Thp.cpp
extern "C" unsigned smgFifoWrites;       // source/compat/GxFifo.cpp
extern "C" int smgDevGxDump();           // source/compat/Kpad.cpp
extern "C" int smgDevGxNull();
extern "C" int smgDevGxTiming();
extern "C" int smgDevGxCheck();
extern "C" void (*smgGpuMemoryChanged)(const void* start, u32 size);  // source/compat/Os.cpp

namespace smg {
namespace {
constexpr std::size_t VertexBytes = 2u << 20, CommandBytes = 1u << 20;
// Triangles are drawn by 16-bit indices from the buffer's start, so it holds no more vertices than they can number.
constexpr std::size_t MaxVertices = VertexBytes / sizeof(GxDrawVertex), MaxIndices = GxVertexPipeline::maxIndices(MaxVertices);
static_assert(MaxVertices <= 0x10000, "vertex numbers are 16 bits");
// Room a draw's commands need at most, in words: when less is left, the frame is ended and begun again (see roomFor).
constexpr u32 DrawCommandWords = 0x800;
// A mesh group's slot rows (up to forty uniforms, four words each) and its draw, beyond the first group's.
constexpr u32 GroupCommandWords = 0x140;

// A vertex shader uniform row, marked for upload only when it differs from what the GPU already holds (citro3d keeps
// the last values): most draws repeat their lights, texgens and material.
inline void setRow(int id, float x, float y, float z, float w) {
    const C3D_FVec& now = C3D_FVUnif[GPU_VERTEX_SHADER][id];
    if (now.x != x || now.y != y || now.z != z || now.w != w) C3D_FVUnifSet(GPU_VERTEX_SHADER, id, x, y, z, w);
}
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

// Which 4 KiB pages some mesh was made from, one bit each over the address space: read without a lock by the flush
// hook, so the game's many flushes of other memory (matrices every frame) cost one load. Set as meshes are made,
// cleared when all are dropped; a stale bit only queues a range for nothing.
std::atomic<std::uint32_t> sMeshPageBits[1u << 15];

// Ranges the game flushed over mesh pages (gxRenderMemoryChanged), kept until the renderer next looks: the hook runs on
// any thread, the renderer only under the FIFO's lock. A leaf lock; past the queue's end every mesh is dropped.
struct ChangedMemory {
    LightLock lock = 1;
    std::uintptr_t start[1024], end[1024];
    unsigned count = 0;
    bool overflowed = false;
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

[[noreturn]] void stop(const char* what) {
    std::fprintf(stderr, "The Wii GPU's drawing: %s is not the port's yet\n", what);
    std::abort();
}

struct Renderer {
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
    // primitives under one state are one draw (flushBatch).
    GxDrawState state;
    PicaPixel pixel;
    int writeMask = 0, scissor[4] = {};
    bool stateValid = false, applied = false, scissorEmpty = false, scissorWhole = true;
    u32 stateBp = 0, stateVersion = 0, appliedVersion = 0;
    GxFrame stateFrame;
    // The target's colour and depth read back (both four bytes a pixel, the screen's way round), until drawn on again.
    u32* readback[2] = {};
    bool fresh[2] = {};
    // A texture by where and how GX fetches it.
    using Key = std::tuple<u32, u16, u16, u8, u8, u8, u8, u8>;
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
        bool ready = false;
    } movie[2];
    GxFrame frame{640, 480, true};
    bool indirectNoted = false, dstAlphaNoted = false;
    // dev.gx_dump: one frame's draws, copies and states listed in the log every minute of the game's clock.
    const GxCommandProcessor* lastGpu = nullptr;
    // The GPU path (gx.v.pica with `transform` on): display lists of primitives kept as meshes in linear memory,
    // each drawn whole with its matrix slots and texgen rows as uniforms.
    int clipUniform = -1, texrowUniform = -1, transformUniform = -1, projectUniform[3] = {-1, -1, -1};
    int materialUniform = -1, rgbRegUniform = -1, alphaRegUniform = -1;
    int ambientUniform = -1, lightUniform = -1, lightLoopUniform = -1, litUniform = -1, rgbLitUniform = -1, alphaLitUniform = -1, anyLightUniform = -1;
    struct CachedMesh {
        std::vector<u32> key;
        std::vector<std::pair<std::uint8_t, u32>> depends;  // the CP registers it was decoded under, and their values
        void* block = nullptr;  // vertices, then indices
        u32 vertexCount = 0, indexCount = 0;
        std::uint16_t slots = 0;
        std::vector<GxMesh::Group> groups;
        std::uintptr_t range[2][2] = {};  // the memory it was made from, [lo, hi): the display list, the array elements
    };
    std::unordered_map<std::uint64_t, CachedMesh> meshes;
    std::unordered_set<std::uintptr_t> meshPages;  // 4 KiB pages some mesh was made from
    std::size_t meshBytes = 0;
    bool dumping = false;
    std::uint32_t gpuXf[5] = {};  // channels, colour/alpha control, material, ambient as the state was built
    unsigned dumpIndex = 0;
    u64 nextDump = 0;
    // Per-frame costs, logged every few seconds (perfLine): what each optimization is measured by.
    struct Perf {
        u64 since = 0, phaseTicks[4] = {}, syncTicks = 0;
        u32 phaseFrames = 0;
        u32 frames = 0, primitives = 0, vertices = 0, triangles = 0, draws = 0, sameState = 0, color0Rides = 0;
        u32 meshDraws = 0, meshTriangles = 0, meshesMade = 0;
        u32 meshInvalidated = 0, meshOverflows = 0, meshBudgetDrops = 0, meshMismatches = 0, flushesQueued = 0;
        u64 timed[6] = {};  // dev.gx_timing: CPU-path primitives (immediate, in lists), display lists (mesh draws), texture copies, movie, frame ends
        u64 cpuCopyTicks = 0;  // of the texture copies, those read back and encoded on the CPU
        u64 stepTicks[6] = {};  // dev.gx_timing: building the draw state, applying it, preparing the vertex pipeline; of the
        u32 steps[6] = {};      // first, reading GX's state, the per-register stats, the pixel state
        u32 cpuCopies = 0, gpuCopies = 0;
        u32 meshesChecked = 0, meshesDiffering = 0;  // dev.gx_check
        u32 listVertices = 0;
        u32 refused[GxVertexPipeline::MeshRefusals] = {}, refusedOpcode[0x100] = {};
        u32 refusedVertices[GxVertexPipeline::MeshRefusals] = {};  // drawn on the CPU path inside lists refused for each reason
        u32 changed[0x100] = {};  // per BP register: state rebuilds it took part in
        u32 compilesBefore = gxTevCompiles(), moviesBefore = smgMovieDecodes(), listsBefore = 0, listBytesBefore = 0, writesBefore = smgFifoWrites;
    } perf;
    // The BP registers a draw reads and the TEV colour and konst words, as of the last state built.
    u32 bpAtBuild[0x100] = {}, colorAtBuild[8] = {}, konstAtBuild[8] = {};

    // Whether C0 is all that changed since the state was built (BP 0xE0-0xE7 hold either kind of colour: by word).
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
        if (clipUniform < 0 || texrowUniform < 0 || transformUniform < 0 || projectUniform[2] < 0 || materialUniform < 0 || alphaRegUniform < 0)
            stop("the GX shader's uniforms");
        // Every row goes to the GPU once, so setRow may skip one whose value citro3d already has.
        for (const auto& [first, count] : {std::pair{clipUniform, 40}, {texrowUniform, 9}, {materialUniform, 1}, {ambientUniform, 1}, {lightUniform, 24}})
            for (int i = 0; i < count; ++i) C3D_FVUnifSet(GPU_VERTEX_SHADER, first + i, 0, 0, 0, 0);
        ready = true;
    }

    void begin() {
        if (!ready) init();
        // Only the GPU's last frame is waited for: the game paces itself by the retrace (waitForRetrace), so a vblank
        // wait here (C3D_FRAME_SYNCDRAW) cost a frame that ran long half a refresh on average. Timed apart.
        const u64 start = svcGetSystemTick();
        C3D_FrameBegin(0);
        perf.syncTicks += svcGetSystemTick() - start;
        C3D_FrameDrawOn(target);
        if (clearPending) C3D_RenderTargetClear(target, C3D_CLEAR_ALL, clearColor, clearDepth);
        fresh[0] = fresh[1] = false;
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
        C3D_BindProgram(&program);
        bindCpuBuffers();
        applied = false;
    }

    // The CPU path's vertices: transformed already, the shader passing them on. Bound again only when a draw from them
    // follows a mesh's (consecutive meshes are most of a scene's draws).
    bool cpuBound = false;
    void ensureCpuBuffers() {
        if (!cpuBound) bindCpuBuffers();
    }
    void bindCpuBuffers() {
        cpuBound = true;
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
    void flushVertices() {
        if (used > flushed) GSPGPU_FlushDataCache(vertices + flushed, (used - flushed) * sizeof(GxDrawVertex));
        if (indexUsed > indexFlushed) GSPGPU_FlushDataCache(indices + indexFlushed, (indexUsed - indexFlushed) * sizeof(std::uint16_t));
        flushed = used, indexFlushed = indexUsed;
    }

    // Ends the frame drawn so far without showing it; resume() begins again on the same target once the GPU is done, so
    // the vertex buffer can start over. Inside a frame citro3d's "sync" transfers only queue: readbacks go between.
    void pause() {
        flushBatch();
        flushVertices();
        C3D_RenderTargetDetachOutput(target);
        C3D_FrameEnd(0);
    }
    void resume() {
        C3D_FrameBegin(0);
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
        const Key key{map.address, map.width, map.height, map.format, map.wrapS, map.wrapT, map.minFilter, map.magFilter};
        if (auto found = textures.find(key); found != textures.end()) return found->second;
        if (map.format == 8 || map.format == 9 || map.format == 10) stop("a palette texture");
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
        if (!upload(texture, tile(source))) stop("a texture the PICA takes no memory for");
        return texture;
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

    void draw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* data) {
        ++perf.primitives, perf.vertices += count;
        ensureState(gpu);
        bool prepared;
        {
            const Step step(perf, 2);
            prepared = pipeline.prepare(gpu, state, frame, kind, format);
        }
        if (!prepared) stop(pipeline.unsupported);
        if (scissorEmpty || !count) return;
        if (!inFrame) begin();
        roomFor(count);
        indexUsed += pipeline.run(data, count, vertices + used, static_cast<std::uint16_t>(used), indices + indexUsed);
        used += count;
    }

    // The draw state for what comes next: kept while the BP registers a draw reads (C0 alone, when it rides) stay.
    void ensureState(const GxCommandProcessor& gpu) {
        if (!stateValid || gpu.bpChanges != stateBp || !(frame == stateFrame)) {
            std::uint8_t color0[4];
            if (stateValid && frame == stateFrame && state.color0InVertex && onlyColor0Changed(gpu) && gxColor0Bytes(gpu, color0))
                stateBp = gpu.bpChanges, ++perf.color0Rides;
            else
                buildState(gpu);
        } else {
            ++perf.sameState;
        }
    }

    // readGxDrawState through a cache keyed by every register it reads (the BP ones a draw reads, the TEV colours and
    // konsts, the channels' XF words): J3D's materials come back every frame, so most builds are a lookup.
    struct CachedState {
        std::vector<u32> words;
        GxDrawState state;
        PicaPixel pixel;  // preparePixel's, of the state's material and program
        bool pixelReady = false;
    };
    std::unordered_map<std::uint64_t, CachedState> states;
    std::vector<u32> stateWords;
    CachedState* stateEntry = nullptr;  // the entry the last read came from or went into

    void readState(const GxCommandProcessor& gpu) {
        static const std::vector<std::uint8_t> registers = [] {
            std::vector<std::uint8_t> read;
            for (u32 a = 0; a < 0x100; ++a)
                if (GxCommandProcessor::drawsRead(a)) read.push_back(std::uint8_t(a));
            return read;
        }();
        stateWords.clear();
        for (std::uint8_t a : registers) stateWords.push_back(gpu.bp[a]);
        for (int i = 0; i < 8; ++i) stateWords.push_back(gpu.tevColor[i]), stateWords.push_back(gpu.tevKonst[i]);
        for (u32 x : {0x1009u, 0x100Au, 0x100Cu, 0x100Eu, 0x1010u, 0x103Fu}) stateWords.push_back(gpu.xf[x]);
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (u32 w : stateWords) hash = (hash ^ w) * 0x100000001B3ull;
        if (auto found = states.find(hash); found != states.end() && found->second.words == stateWords) {
            stateEntry = &found->second;
            return void(state = found->second.state);
        }
        readGxDrawState(gpu, state);
        if (states.size() >= 512) states.clear();
        CachedState& entry = states[hash];
        entry.words = stateWords, entry.state = state, entry.pixelReady = false;
        stateEntry = &entry;
    }

    // dev.gx_timing: the clock around one of the per-call steps.
    struct Step {
        Step(Perf& perf, int which) : perf_(perf), which_(which), start_(timing() ? svcGetSystemTick() : 0) {}
        ~Step() {
            if (start_) perf_.stepTicks[which_] += svcGetSystemTick() - start_, ++perf_.steps[which_];
        }
        static bool timing() {
            static const bool on = smgDevGxTiming();
            return on;
        }
        Perf& perf_;
        int which_;
        u64 start_;
    };

    void buildState(const GxCommandProcessor& gpu) {
        flushBatch();
        const Step step(perf, 0);
        {
            const Step read(perf, 3);
            readState(gpu);
        }
        pipeline.stateChanged();
        if (Step::timing()) {
            const Step stats(perf, 4);
            for (u32 a = 0; a < 0x100; ++a)
                if (gpu.bp[a] != bpAtBuild[a] && GxCommandProcessor::drawsRead(a)) ++perf.changed[a];
        }
        std::memcpy(bpAtBuild, gpu.bp, sizeof bpAtBuild);
        std::memcpy(colorAtBuild, gpu.tevColor, sizeof colorAtBuild);
        std::memcpy(konstAtBuild, gpu.tevKonst, sizeof konstAtBuild);
        stateValid = true, stateBp = gpu.bpChanges, stateFrame = frame, ++stateVersion;
        if (state.unsupported) stop(state.unsupported);
        if (state.material.blend.type == 2 && state.material.blend.logic != 3) stop("a logic op other than copy");
        if (state.depthTextureOp) depthFromTexture(state);
        if (state.indirectDropped && !indirectNoted) {
            std::fprintf(stderr, "The Wii GPU's drawing: indirect texture offsets are left out (the PICA has no dependent reads)\n");
            indirectNoted = true;
        }
        // GX's scissor in screen pixels, then on the target turned a quarter as the LCD is (citro3d's convention).
        const int x0 = std::clamp(static_cast<int>(std::lround(state.scissor[0] * 400.0f / frame.width)), 0, 400);
        const int x1 = std::clamp(static_cast<int>(std::lround(state.scissor[2] * 400.0f / frame.width)), 0, 400);
        const int y0 = std::clamp(static_cast<int>(std::lround(state.scissor[1] * 240.0f / frame.height)), 0, 240);
        const int y1 = std::clamp(static_cast<int>(std::lround(state.scissor[3] * 240.0f / frame.height)), 0, 240);
        scissorEmpty = x1 <= x0 || y1 <= y0;
        scissorWhole = x0 == 0 && y0 == 0 && x1 == 400 && y1 == 240;
        scissor[0] = 240 - y1, scissor[1] = 400 - x1, scissor[2] = 240 - y0, scissor[3] = 400 - x0;
        gpuXf[0] = gpu.xf[0x1009], gpuXf[1] = gpu.xf[0x100E], gpuXf[2] = gpu.xf[0x1010], gpuXf[3] = gpu.xf[0x100C], gpuXf[4] = gpu.xf[0x100A];
        {
            const Step prepare(perf, 5);
            if (stateEntry->pixelReady) {
                pixel = stateEntry->pixel;
            } else {
                pixel = preparePixel(state.material, state.program);
                stateEntry->pixel = pixel, stateEntry->pixelReady = true;
            }
        }
        if (state.dstAlphaEnable && state.alphaUpdate) keepDstAlpha();
        writeMask = (state.colorUpdate ? GPU_WRITE_COLOR & ~GPU_WRITE_ALPHA : 0) | (state.alphaUpdate ? GPU_WRITE_ALPHA : 0) |
                    (pixel.depthWrite ? GPU_WRITE_DEPTH : 0);
    }

    // GXSetDstAlpha: the EFB takes a constant alpha while the TEV's blends and is tested. The PICA blends and tests with
    // the alpha it writes, so that holds exactly only when neither reads the source alpha; then a last stage replaces it.
    void keepDstAlpha() {
        const auto readsSource = [](GPU_BLENDFACTOR f) { return f == GPU_SRC_ALPHA || f == GPU_ONE_MINUS_SRC_ALPHA; };
        if (readsSource(pixel.source) || readsSource(pixel.destination) || pixel.alphaTest) {
            if (!dstAlphaNoted) std::fprintf(stderr, "The Wii GPU's drawing: destination alpha with a blend or test that reads the source alpha keeps the source alpha\n");
            dstAlphaNoted = true;
            return;
        }
        const unsigned last = std::min(state.program.stageCount, CombinerProgram::MaxStages - 1);
        C3D_TexEnv& env = pixel.env[last];
        if (last == state.program.stageCount) {
            C3D_TexEnvInit(&env);
            C3D_TexEnvSrc(&env, C3D_RGB, GPU_PREVIOUS);
        } else if ((env.srcRgb & 0xF) == GPU_CONSTANT || (env.srcRgb >> 4 & 0xF) == GPU_CONSTANT || (env.srcRgb >> 8 & 0xF) == GPU_CONSTANT) {
            if (!dstAlphaNoted) std::fprintf(stderr, "The Wii GPU's drawing: destination alpha on a sixth stage that reads its constant shares the constant\n");
            dstAlphaNoted = true;
        }
        C3D_TexEnvSrc(&env, C3D_Alpha, GPU_CONSTANT);
        C3D_TexEnvOpAlpha(&env, GPU_TEVOP_A_SRC_ALPHA);
        C3D_TexEnvFunc(&env, C3D_Alpha, GPU_REPLACE);
        env.color = (env.color & 0x00FFFFFF) | u32(state.dstAlpha) << 24;
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
        const ModelMaterial& m = state.material;
        std::string stages;
        for (int s = 0; s < m.tevStageCount; ++s) {
            const TevStage& t = m.stages[s];
            char text[96];
            std::snprintf(text, sizeof text, " [%x%x%x%x op%u %x%x%x%x op%u map%d ch%u]", t.colorIn[0], t.colorIn[1], t.colorIn[2], t.colorIn[3], t.colorOp,
                          t.alphaIn[0], t.alphaIn[1], t.alphaIn[2], t.alphaIn[3], t.alphaOp, t.texMap == 0xFF ? -1 : t.texMap, t.channel);
            stages += text;
        }
        std::string maps;
        for (unsigned u = 0; u < state.program.unitCount; ++u) {
            const GxTextureMap& t = state.textures[state.program.units[u].texture];
            char text[64];
            std::snprintf(text, sizeof text, " %08lx:%ux%u:f%u", static_cast<unsigned long>(t.address), t.width, t.height, t.format);
            maps += text;
        }
        // The colours the combiner reads: konst and C0, and what the vertices' colour (the rasterized channel) came out as.
        unsigned colourLo[4] = {255, 255, 255, 255}, colourHi[4] = {}, colourSum[4] = {}, counted = 0;
        for (std::size_t i = batchStart; i < indexUsed && counted < 4096; ++i, ++counted)
            for (int c = 0; c < 4; ++c) {
                const unsigned v = vertices[indices[i]].color[c];
                colourLo[c] = std::min(colourLo[c], v), colourHi[c] = std::max(colourHi[c], v), colourSum[c] += v;
            }
        const auto byte = [](float v) { return int(std::lround(v * 255)); };
        std::string konst;
        for (int k = 0; k < 4; ++k) {
            char text[48];
            std::snprintf(text, sizeof text, " K%d=%d,%d,%d,%d", k, byte(m.konstColors[k][0]), byte(m.konstColors[k][1]), byte(m.konstColors[k][2]),
                          byte(m.konstColors[k][3]));
            konst += text;
        }
        std::fprintf(stderr, "gx dump:   colours: vertex lo %u,%u,%u,%u avg %u,%u,%u,%u hi %u,%u,%u,%u; C0=%d,%d,%d,%d%s; ksel %x/%x; xf chans %lu ctrl "
                     "%08lx/%08lx mat %08lx amb %08lx\n", colourLo[0], colourLo[1], colourLo[2], colourLo[3], counted ? colourSum[0] / counted : 0,
                     counted ? colourSum[1] / counted : 0, counted ? colourSum[2] / counted : 0, counted ? colourSum[3] / counted : 0, colourHi[0],
                     colourHi[1], colourHi[2], colourHi[3], byte(m.tevColors[0][0]), byte(m.tevColors[0][1]), byte(m.tevColors[0][2]),
                     byte(m.tevColors[0][3]), konst.c_str(), m.stages[0].konstColor, m.stages[0].konstAlpha, static_cast<unsigned long>(gpuXf[0]),
                     static_cast<unsigned long>(gpuXf[1]), static_cast<unsigned long>(gpuXf[2]), static_cast<unsigned long>(gpuXf[3]),
                     static_cast<unsigned long>(gpuXf[4]));
        std::fprintf(stderr, "gx dump: draw %u: %u tris, box (%.0f,%.0f)-(%.0f,%.0f); blend %u %u/%u, colour %d alpha %d, dst alpha %d:%u, z %d/%u/%d, "
                     "alpha test %u%s%u, C0 %s; tev%s; maps%s%s\n", dumpIndex++, unsigned((indexUsed - batchStart) / 3), lo[0], lo[1], hi[0], hi[1],
                     m.blend.type, m.blend.source, m.blend.destination, state.colorUpdate, state.alphaUpdate, state.dstAlphaEnable, state.dstAlpha,
                     m.depth.enable, m.depth.function, m.depth.update, m.alphaTest.compare0, m.alphaTest.op ? "|" : "&", m.alphaTest.compare1,
                     state.color0InVertex ? "rides" : "const", stages.c_str(), maps.c_str(), state.program.exact ? "" : " (approximated)");
        return lo[0] <= 1 && lo[1] <= 1 && hi[0] >= frame.width - 1 && hi[1] >= frame.height - 1;
    }

    // A mesh draw for the dump: its size and the state that draws it.
    void dumpMesh(const CachedMesh& mesh, const GxMeshUniforms& u) {
        const ModelMaterial& m = state.material;
        std::string stages;
        for (int s = 0; s < m.tevStageCount; ++s) {
            const TevStage& t = m.stages[s];
            char text[96];
            std::snprintf(text, sizeof text, " [%x%x%x%x op%u %x%x%x%x op%u map%d ch%u]", t.colorIn[0], t.colorIn[1], t.colorIn[2], t.colorIn[3], t.colorOp,
                          t.alphaIn[0], t.alphaIn[1], t.alphaIn[2], t.alphaIn[3], t.alphaOp, t.texMap == 0xFF ? -1 : t.texMap, t.channel);
            stages += text;
        }
        std::fprintf(stderr, "gx dump: draw %u (mesh): %u tris in %zu groups, lit %d (%u entries, rgb %d alpha %d), cull %u; blend %u %u/%u, colour %d "
                     "alpha %d, dst alpha %d:%u, z %d/%u/%d, alpha test %u%s%u; tev%s\n", dumpIndex++, mesh.indexCount / 3, mesh.groups.size(), u.lit,
                     u.lights, u.rgbLit, u.alphaLit, u.reject, m.blend.type, m.blend.source, m.blend.destination, state.colorUpdate, state.alphaUpdate,
                     state.dstAlphaEnable, state.dstAlpha, m.depth.enable, m.depth.function, m.depth.update, m.alphaTest.compare0,
                     m.alphaTest.op ? "|" : "&", m.alphaTest.compare1, stages.c_str());
    }

    static constexpr std::size_t MeshBudget = 12u << 20;

    void dropMesh(std::unordered_map<std::uint64_t, CachedMesh>::iterator it) {
        meshBytes -= it->second.vertexCount * sizeof(GxMeshVertex) + it->second.indexCount * sizeof(std::uint16_t);
        linearFree(it->second.block);
        meshes.erase(it);
    }
    void dropAllMeshes() {
        if (inFrame) pause(), resume();  // the GPU may still read them
        while (!meshes.empty()) dropMesh(meshes.begin());
        meshPages.clear();
        for (auto& bits : sMeshPageBits) bits.store(0, std::memory_order_relaxed);
    }

    // The CPU wrote memory the GPU will read (the game's cache flushes, noted in sChanged): meshes made from it go.
    std::uintptr_t changedStart[1024], changedEnd[1024];
    void applyChangedMemory() {
        std::uintptr_t* start = changedStart;
        std::uintptr_t* end = changedEnd;
        LightLock_Lock(&sChanged.lock);
        const unsigned count = sChanged.count;
        const bool overflowed = sChanged.overflowed;
        std::memcpy(start, sChanged.start, count * sizeof *start), std::memcpy(end, sChanged.end, count * sizeof *end);
        sChanged.count = 0, sChanged.overflowed = false;
        LightLock_Unlock(&sChanged.lock);
        perf.flushesQueued += count;
        if (meshes.empty()) return;
        if (overflowed) return ++perf.meshOverflows, dropAllMeshes();
        bool paused = false;
        for (unsigned i = 0; i < count; ++i) {
            bool touched = false;
            for (std::uintptr_t page = start[i] >> 12; page <= (end[i] - 1) >> 12 && !touched; ++page) touched = meshPages.count(page);
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
        }
    }

    // GX_CallDisplayList: a list of primitives under a state the GPU path covers is drawn from its mesh (made the first
    // time); anything else is read by the command processor as before.
    bool displayList(const GxCommandProcessor& gpu, std::uint32_t address, std::uint32_t size) {
        const unsigned why = displayListMesh(gpu, address, size);
        listRefusal = why;
        return !why;
    }
    unsigned listRefusal = 0;

    // 0 when the list was drawn from a mesh, else why not.
    unsigned displayListMesh(const GxCommandProcessor& gpu, std::uint32_t address, std::uint32_t size) {
        const std::uint8_t* list = gpu.memoryFunction()(address);
        if (!list || size < 3) return GxVertexPipeline::MeshUnsupported;
        if (list[0] < 0x80 || list[0] >= 0xA8) return ++perf.refusedOpcode[list[0]], ++perf.refused[GxVertexPipeline::MeshCommand], GxVertexPipeline::MeshCommand;
        applyChangedMemory();
        ensureState(gpu);
        bool prepared;
        {
            const Step step(perf, 2);
            prepared = pipeline.prepare(gpu, state, frame, 0x90, 0);
        }
        if (!prepared) return ++perf.refused[GxVertexPipeline::MeshUnsupported], GxVertexPipeline::MeshUnsupported;
        if (scissorEmpty) return GxVertexPipeline::MeshEmpty;
        if (const unsigned why = pipeline.meshEligible()) return ++perf.refused[why], why;
        // The key: the list, its first and last words (another list at a freed address differs), where the colour comes
        // from and the texgens' sources. The CP registers its vertices were read under are checked apart, only those it
        // uses: other formats' tables and the matrix arrays (12-15) change every frame without touching it.
        u32 key[7];
        unsigned n = 0;
        key[n++] = address, key[n++] = size, key[n++] = pipeline.meshColorFromVertex(), key[n++] = pipeline.meshSources();
        key[n++] = pipeline.meshLit();  // lit meshes number their slots another way
        std::memcpy(&key[n], list, 4), std::memcpy(&key[n + 1], list + size - 4, 4), n += 2;
        std::uint64_t hash = 0xCBF29CE484222325ull;
        for (unsigned i = 0; i < n; ++i) hash = (hash ^ key[i]) * 0x100000001B3ull;
        auto found = meshes.find(hash);
        bool stale = found != meshes.end() && (found->second.key.size() != n || std::memcmp(found->second.key.data(), key, n * 4));
        if (found != meshes.end() && !stale)
            for (const auto& [reg, value] : found->second.depends) stale |= gpu.cp[reg] != value;
        if (stale) dropMesh(found), found = meshes.end(), ++perf.meshMismatches;
        if (found == meshes.end()) {
            GxMesh mesh;
            if (const unsigned why = pipeline.buildMesh(list, size, mesh)) return ++perf.refused[why], why;
            // Its pages are marked before it is read for good: a write and flush from another thread between a reading and
            // the marking would never reach it (the flush hook passes unmarked pages by), so the first reading only finds them.
            markPages(reinterpret_cast<std::uintptr_t>(list), reinterpret_cast<std::uintptr_t>(list) + size);
            markPages(reinterpret_cast<std::uintptr_t>(mesh.arraysLo), reinterpret_cast<std::uintptr_t>(mesh.arraysHi));
            std::atomic_thread_fence(std::memory_order_seq_cst);
            mesh = GxMesh{};
            if (const unsigned why = pipeline.buildMesh(list, size, mesh)) return ++perf.refused[why], why;
            static const bool checking = smgDevGxCheck();
            if (checking) checkMesh(gpu, list, size, mesh, address);
            const std::size_t vertexBytes = mesh.vertices.size() * sizeof(GxMeshVertex), bytes = vertexBytes + mesh.indices.size() * sizeof(std::uint16_t);
            if (meshBytes + bytes > MeshBudget) dropAllMeshes(), ++perf.meshBudgetDrops;
            void* block = linearAlloc(bytes);
            if (!block && !meshes.empty()) dropAllMeshes(), block = linearAlloc(bytes);
            if (!block) return GxVertexPipeline::MeshTooLarge;
            std::memcpy(block, mesh.vertices.data(), vertexBytes);
            std::memcpy(static_cast<std::uint8_t*>(block) + vertexBytes, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint16_t));
            GSPGPU_FlushDataCache(block, bytes);
            CachedMesh& cached = meshes[hash];
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
            cached.block = block, cached.vertexCount = u32(mesh.vertices.size()), cached.indexCount = u32(mesh.indices.size());
            cached.slots = mesh.slots;
            cached.groups = std::move(mesh.groups);
            // Apart: a list and its arrays sit in different heaps, and one span over both would take in whatever lies between.
            cached.range[0][0] = reinterpret_cast<std::uintptr_t>(list), cached.range[0][1] = cached.range[0][0] + size;
            cached.range[1][0] = reinterpret_cast<std::uintptr_t>(mesh.arraysLo), cached.range[1][1] = reinterpret_cast<std::uintptr_t>(mesh.arraysHi);
            if (cached.range[1][1] <= cached.range[1][0]) cached.range[1][0] = cached.range[1][1] = 0;  // all attributes direct
            for (const auto& r : cached.range) markPages(r[0], r[1]);
            meshBytes += bytes, ++perf.meshesMade;
            found = meshes.find(hash);
        }
        drawMesh(found->second);
        return GxVertexPipeline::MeshOk;
    }

    void markPages(std::uintptr_t lo, std::uintptr_t hi) {
        if (hi <= lo) return;
        for (std::uintptr_t page = lo >> 12; page <= (hi - 1) >> 12; ++page) {
            meshPages.insert(page);
            sMeshPageBits[page >> 5].fetch_or(1u << (page & 31), std::memory_order_relaxed);
        }
    }

    // dev.gx_check: a new mesh against the CPU path, vertex by vertex, under the state it is first drawn with (each
    // group's uniforms through gxMeshModel); a mesh's first difference is logged.
    void checkMesh(const GxCommandProcessor& gpu, const std::uint8_t* list, std::uint32_t size, const GxMesh& mesh, std::uint32_t address) {
        ++perf.meshesChecked;
        std::vector<GxMeshUniforms> uniforms(mesh.groups.size());
        pipeline.meshUniforms(mesh.slots, mesh.groups[0], uniforms[0]);
        for (std::size_t g = 1; g < mesh.groups.size(); ++g) uniforms[g] = uniforms[0], pipeline.meshSlotUniforms(mesh.groups[g], uniforms[g]);
        std::vector<u32> group(mesh.vertices.size(), 0);
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
            if (!pipeline.prepare(gpu, state, frame, op & 0xF8, op & 7)) return void(std::fprintf(stderr, "gx check: the CPU path refuses a mesh's primitive\n"));
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
        pipeline.prepare(gpu, state, frame, 0x90, 0);
        const auto off = [](float got, float want) { return std::fabs(got - want) > 1e-3f * std::max(1.0f, std::fabs(want)); };
        for (std::size_t j = 0; j < mesh.vertices.size(); ++j) {
            if (mesh.origin[j] >= cpu.size()) return void(std::fprintf(stderr, "gx check: a mesh vertex from past its list's\n"));
            const GxDrawVertex& want = cpu[mesh.origin[j]];
            const GxMeshUniforms& u = uniforms[group[j]];
            float clip[4], color[4], tex[CombinerProgram::MaxUnits][2];
            gxMeshModel(u, mesh.vertices[j], clip, color, tex);
            const char* what = nullptr;
            for (int r = 0; r < 4 && !what; ++r)
                if (off(clip[r], want.position[r])) what = "position";
            for (int c = 0; c < 4 && !what; ++c)
                if (std::abs(int(std::lround(color[c] * 255)) - int(want.color[c])) > 2) what = "colour";
            for (unsigned t = 0; t < u.units && !what; ++t)
                if (off(tex[t][0], want.texcoord[t][0]) || off(tex[t][1], want.texcoord[t][1])) what = "texture coordinates";
            if (!what) continue;
            ++perf.meshesDiffering;
            const GxMesh::Group& in = mesh.groups[group[j]];
            std::fprintf(stderr, "gx check: mesh at 0x%08lx (%zu vertices, %zu groups, lit %d, %u light entries, %u units, slots 0x%x) vertex %zu "
                         "(list vertex %lu, group %lu of slots %u %u %u %u, slot row %.0f) differs in %s: position %.3f %.3f %.3f %.3f vs %.3f %.3f "
                         "%.3f %.3f; colour %.0f %.0f %.0f %.0f vs %u %u %u %u; first texcoords %.3f %.3f vs %.3f %.3f; normal %.3f %.3f %.3f\n",
                         static_cast<unsigned long>(address), mesh.vertices.size(), mesh.groups.size(), u.lit, u.lights, u.units, mesh.slots, j,
                         static_cast<unsigned long>(mesh.origin[j]), static_cast<unsigned long>(group[j]), in.slotMap[0], in.slotMap[1], in.slotMap[2],
                         in.slotMap[3], mesh.vertices[j].slot, what, clip[0], clip[1], clip[2], clip[3], want.position[0], want.position[1],
                         want.position[2], want.position[3], color[0] * 255, color[1] * 255, color[2] * 255, color[3] * 255, want.color[0], want.color[1],
                         want.color[2], want.color[3], tex[0][0], tex[0][1], want.texcoord[0][0], want.texcoord[0][1], mesh.vertices[j].normal[0],
                         mesh.vertices[j].normal[1], mesh.vertices[j].normal[2]);
            return;
        }
    }

    void drawMesh(const CachedMesh& mesh) {
        if (!inFrame) begin();
        flushBatch();
        roomFor(0, DrawCommandWords + u32(mesh.groups.size() - 1) * GroupCommandWords);
        applyState();
        GxMeshUniforms u;
        pipeline.meshUniforms(mesh.slots, mesh.groups[0], u);
        if (dumping) dumpMesh(mesh, u);
        if (u.reject == 3) return;  // GX culls every triangle
        slotUniforms(u);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, litUniform, u.lit);
        if (u.lit) {
            for (unsigned l = 0; l < u.lights; ++l)
                for (int r = 0; r < 6; ++r) {
                    const float* row = u.light[l][r];
                    setRow(lightUniform + int(l) * 6 + r, row[0], row[1], row[2], row[3]);
                }
            setRow(ambientUniform, u.ambient[0], u.ambient[1], u.ambient[2], u.ambient[3]);
            C3D_IVUnifSet(GPU_VERTEX_SHADER, lightLoopUniform, u.lights ? int(u.lights) - 1 : 0, 0, 6, 0);
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, anyLightUniform, u.lights != 0);
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, rgbLitUniform, u.rgbLit);
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, alphaLitUniform, u.alphaLit);
        }
        for (unsigned unit = 0; unit < u.units; ++unit) {
            for (int r = 0; r < 3; ++r) {
                const float* row = u.texgen[unit][r];
                setRow(texrowUniform + int(unit) * 3 + r, row[0], row[1], row[2], row[3]);
            }
            C3D_BoolUnifSet(GPU_VERTEX_SHADER, projectUniform[unit], u.projective[unit]);
        }
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, transformUniform, true);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, rgbRegUniform, !u.fixedColor && !u.rgbFromVertex);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, alphaRegUniform, !u.fixedColor && !u.alphaFromVertex);
        setRow(materialUniform, u.color[0] / 255.0f, u.color[1] / 255.0f, u.color[2] / 255.0f, u.color[3] / 255.0f);
        // GxMeshVertex: position (v0), colour (v1), three texgen inputs (v2-v4), normal (v6), slot (v5). What a draw doesn't
        // take from the vertices is a fixed attribute, and the bytes it skips are padding (0xC is four bytes, 0xE twelve).
        C3D_AttrInfo* attributes = C3D_GetAttrInfo();
        AttrInfo_Init(attributes);
        u64 permutation = 0;
        int components = 0;
        const auto loader = [&](int reg, GPU_FORMATS format, int count) {
            permutation |= u64(AttrInfo_AddLoader(attributes, reg, format, count)) << (4 * components++);
        };
        const auto pad = [&](unsigned code) { permutation |= u64(code) << (4 * components++); };
        const auto fixed = [&](int reg, float x, float y, float z, float w) {
            AttrInfo_AddFixed(attributes, reg);
            C3D_FVec* value = C3D_FixedAttribGetWritePtr(reg);
            value->x = x, value->y = y, value->z = z, value->w = w;
        };
        loader(0, GPU_FLOAT, 3);
        if (u.fixedColor) pad(0xC), fixed(1, u.color[0], u.color[1], u.color[2], u.color[3]);
        else loader(1, GPU_UNSIGNED_BYTE, 4);
        for (unsigned unit = 0; unit < 3; ++unit) {
            if (unit < u.units) loader(2 + int(unit), GPU_FLOAT, 3);
            else pad(0xE), fixed(2 + int(unit), 0, 0, 0, 1);
        }
        if (u.lit) loader(6, GPU_FLOAT, 3);
        else pad(0xE), fixed(6, 0, 0, 0, 0);
        if (mesh.slots) loader(5, GPU_FLOAT, 1);
        else fixed(5, 0, 0, 0, 0);
        C3D_BufInfo* buffers = C3D_GetBufInfo();
        BufInfo_Init(buffers);
        BufInfo_Add(buffers, mesh.block, sizeof(GxMeshVertex), components, permutation);
        // GX culls in the EFB, its y down; the PICA sees the same triangles turned for the screen.
        C3D_CullFace(u.reject == 1 ? GPU_CULL_FRONT_CCW : u.reject == 2 ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);
        const auto* indices = reinterpret_cast<const std::uint16_t*>(static_cast<std::uint8_t*>(mesh.block) + mesh.vertexCount * sizeof(GxMeshVertex));
        for (std::size_t g = 0; g < mesh.groups.size(); ++g) {
            if (g) pipeline.meshSlotUniforms(mesh.groups[g], u), slotUniforms(u);
            C3D_DrawElements(GPU_TRIANGLES, int(mesh.groups[g].indexCount), C3D_UNSIGNED_SHORT, indices + mesh.groups[g].firstIndex);
        }
        u32* buffer;
        u32 size, offset;
        GPUCMD_GetBuffer(&buffer, &size, &offset);
        if (size - offset < 0x40) stop("a mesh's draws filling the GPU's command list");  // GPUCMD_Add would have dropped some
        C3D_CullFace(GPU_CULL_NONE);
        cpuBound = false;
        fresh[0] = fresh[1] = false;
        ++perf.meshDraws, perf.meshTriangles += mesh.indexCount / 3;
    }

    // Each slot's rows: four of the clip matrix unlit; lit, ten (clip, modelview, normal) for each compact slot.
    void slotUniforms(const GxMeshUniforms& u) {
        const int rows = u.lit ? 10 : 4;
        for (unsigned slot = 0; slot < 10; ++slot) {
            if (!(u.slots >> slot & 1)) continue;
            const int at = clipUniform + int(slot) * rows;
            for (int r = 0; r < 4; ++r) {
                const float* row = u.clip[slot] + r * 4;
                setRow(at + r, row[0], row[1], row[2], row[3]);
            }
            if (!u.lit) continue;
            for (int r = 0; r < 3; ++r) {
                const float* row = u.modelView[slot] + r * 4;
                setRow(at + 4 + r, row[0], row[1], row[2], row[3]);
                const float* normal = u.normal[slot] + r * 3;
                setRow(at + 7 + r, normal[0], normal[1], normal[2], 0);
            }
        }
    }

    // The pending primitives as one draw, under the state they share.
    void flushBatch() {
        if (indexUsed == batchStart) return;
        applyState();
        const bool wholeScreen = dumping && dumpBatch();
        ensureCpuBuffers();
        C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(indexUsed - batchStart), C3D_UNSIGNED_SHORT, indices + batchStart);
        ++perf.draws, perf.triangles += (indexUsed - batchStart) / 3;
        batchStart = indexUsed;
        fresh[0] = fresh[1] = false;
        if (wholeScreen) dumpEfbAverage("after it");
    }

    void applyState() {
        const Step step(perf, 1);
        if (!applied || appliedVersion != stateVersion) {
            pixel.apply();
            C3D_DepthTest(pixel.depthTest, pixel.depthTest ? pixel.depthFunc : GPU_ALWAYS, static_cast<GPU_WRITEMASK>(writeMask));
            C3D_CullFace(GPU_CULL_NONE);  // culled in EFB space already
            if (scissorWhole) C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
            else C3D_SetScissor(GPU_SCISSOR_NORMAL, scissor[0], scissor[1], scissor[2], scissor[3]);
            for (unsigned unit = 0; unit < state.program.unitCount; ++unit)
                C3D_TexBind(unit, &texture(state.textures[state.program.units[unit].texture]));
            applied = true, appliedVersion = stateVersion;
        }
    }

    // The EFB's average colour so far, read back (dumps only): which full-screen pass changes the picture, and how.
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
        if (size - offset >= words && used + vertexCount <= MaxVertices && indexUsed + GxVertexPipeline::maxIndices(vertexCount) <= MaxIndices)
            return;
        if (vertexCount > MaxVertices) stop("a primitive with more vertices than the port's buffer");
        pause();
        resume();
    }

    void perfLine() {
        const u64 now = svcGetSystemTick();
        ++perf.frames;
        if (!perf.since) perf.since = now;
        const double ms = double(now - perf.since) * 1000.0 / SYSCLOCK_ARM11;
        if (ms < 4000) return;
        const double n = perf.frames;
        const double phase = perf.phaseFrames ? 1000.0 / SYSCLOCK_ARM11 / perf.phaseFrames : 0;
        // t= is the emulated clock (drives time their actions by it when Azahar runs faster than real time).
        std::fprintf(stderr, "gx perf: t=%.0f, %lu frames, %.1f ms/frame: draw %.1f (%.1f of it waiting for the GPU), logic %.1f, "
                     "animation %.1f, retrace wait %.1f; per frame %.0f primitives (%.0f same state, %.0f with only C0 new), %.0f vertices, "
                     "%.0f triangles, %.0f draws\n",
                     double(now) / SYSCLOCK_ARM11, static_cast<unsigned long>(perf.frames), ms / n, perf.phaseTicks[0] * phase, perf.syncTicks * 1000.0 / SYSCLOCK_ARM11 / n,
                     perf.phaseTicks[1] * phase, perf.phaseTicks[2] * phase, perf.phaseTicks[3] * phase, perf.primitives / n,
                     perf.sameState / n, perf.color0Rides / n, perf.vertices / n, perf.triangles / n, perf.draws / n);
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
        std::fprintf(stderr, "gx perf: GPU path %.0f draws, %.0f triangles, %.1f meshes made per frame (%zu kept, %.1f MB); TEV programs compiled %.1f, "
                     "movie frames decoded %.1f, display lists %.0f (%.0f bytes) per frame; %.1f MB linear free; state changes by BP register (per frame):%s\n",
                     perf.meshDraws / n, perf.meshTriangles / n, perf.meshesMade / n, meshes.size(), meshBytes / 1048576.0, (gxTevCompiles() - perf.compilesBefore) / n,
                     (smgMovieDecodes() - perf.moviesBefore) / n, (lastGpu->displayListCalls - perf.listsBefore) / n,
                     (lastGpu->displayListBytes - perf.listBytesBefore) / n, linearSpaceFree() / 1048576.0, top.c_str());
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
                         "pipeline %.1f ms (%.0f); building: reading GX %.1f ms, stats %.1f ms, pixel state %.1f ms\n", perf.stepTicks[0] * ms,
                         perf.steps[0] / n, perf.stepTicks[1] * ms, perf.steps[1] / n, perf.stepTicks[2] * ms, perf.steps[2] / n, perf.stepTicks[3] * ms,
                         perf.stepTicks[4] * ms, perf.stepTicks[5] * ms);
        }
        std::string listed;
        for (unsigned why = 0; why < GxVertexPipeline::MeshRefusals; ++why)
            if (perf.refusedVertices[why]) listed += " " + std::to_string(why) + ":" + std::to_string(unsigned(perf.refusedVertices[why] / n));
        std::fprintf(stderr, "gx perf: CPU-path vertices in lists refused by reason (per frame):%s\n", listed.c_str());
        if (perf.meshesChecked) std::fprintf(stderr, "gx perf: meshes checked %lu, differing %lu\n", static_cast<unsigned long>(perf.meshesChecked),
                                             static_cast<unsigned long>(perf.meshesDiffering));
        std::fprintf(stderr, "gx perf: display lists refused by reason (per frame):%s; meshes invalidated %.1f, overflows %.1f, over budget %.1f, "
                     "key mismatches %.1f, flushes queued %.1f; GX FIFO writes %.0f per frame\n", refusals.c_str(), perf.meshInvalidated / n,
                     perf.meshOverflows / n, perf.meshBudgetDrops / n, perf.meshMismatches / n, perf.flushesQueued / n, (smgFifoWrites - perf.writesBefore) / n);
        perf = Perf{};
        perf.since = now, perf.listsBefore = lastGpu->displayListCalls, perf.listBytesBefore = lastGpu->displayListBytes;
    }

    // Textures made from memory a copy has just written are stale.
    void forget(u32 address, std::size_t size) {
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
            const Efb efb = efbView(gpu);
            encodeTextureCopy(copy, efbPixel, &efb, static_cast<std::uint8_t*>(OSPhysicalToCached(copy.address)));
            perf.cpuCopyTicks += svcGetSystemTick() - start;
            static unsigned noted = 0;
            if (dumping || noted++ < 4)
                std::fprintf(stderr, "gx perf: CPU copy (%u,%u %ux%u) to 0x%08lx format %u took %.2f ms\n", copy.x, copy.y, copy.width, copy.height,
                             static_cast<unsigned long>(copy.address), copy.format, double(svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11);
        }
        forget(copy.address, textureCopySize(copy));
        stateValid = applied = false;  // its textures may be gone
        if (copy.clear) C3D_RenderTargetClear(target, C3D_CLEAR_ALL, copyClearColor(gpu), copyClearDepth(gpu)), fresh[0] = fresh[1] = false;
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
        int rows[3];
        std::uint8_t weights[3];
        unsigned taps = 0;
        for (int i = 0; i < 7; ++i) {
            if (!copy.filter[i]) continue;
            if (taps == 3) return false;  // the PICA samples three textures at once
            rows[taps] = i - 3, weights[taps++] = copy.filter[i];
        }
        const bool noAlpha = PE_CONTROL_GET_PIXTYPE(gpu.bp[0x43]) == 0;
        if (!taps || copyStages(copy, taps, noAlpha) > CombinerProgram::MaxStages) return false;
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
            if (!C3D_TexInitVRAM(&scratch, 256, 512, GPU_RGBA8) && !C3D_TexInit(&scratch, 256, 512, GPU_RGBA8))
                stop("an EFB copy with no memory to take the target into");
            C3D_TexSetFilter(&scratch, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&scratch, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            scratchReady = true;
        }
        roomFor(18);
        // Queued behind the draws so far (inside a frame citro3d's "sync" copy does not wait), as the pass is behind it.
        C3D_SyncTextureCopy(static_cast<u32*>(target->frameBuf.colorBuf), GX_BUFFER_DIM(TargetTileRow / 16, 0), static_cast<u32*>(scratch.data),
                            GX_BUFFER_DIM(TargetTileRow / 16, (ScratchTileRow - TargetTileRow) / 16), TargetTileRow * 50, GX_TRANSFER_RAW_COPY(1));

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
        copyCombiner(copy, taps, weights, noAlpha);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        for (int unit = 0; unit < 3; ++unit) C3D_TexBind(unit, &scratch);
        ensureCpuBuffers();
        C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(used), static_cast<int>(n));
        used += n;
        applied = false;
        C3D_FrameDrawOn(target);
        noteGpuCopy(gpu, copy);
        return true;
    }

    // Once per format and size, the GPU's copy is read back and set against encodeTextureCopy's from the same target,
    // as drawn and mirrored each way: the log shows the PICA's orientation and channels agree (or how they don't).
    void noteGpuCopy(const GxCommandProcessor& gpu, const GxTextureCopy& copy) {
        const u64 kind = u64(copy.format) | u64(copy.intensity) << 4 | u64(copy.halfScale) << 5 | u64(copy.texWidth) << 8 | u64(copy.texHeight) << 24;
        if (std::find(checkedKinds.begin(), checkedKinds.end(), kind) != checkedKinds.end()) return;
        checkedKinds.push_back(kind);
        pause(), resume();  // the GPU has drawn the copy's quad
        const Efb efb = efbView(gpu);
        std::vector<std::uint8_t> bytes(textureCopySize(copy));
        encodeTextureCopy(copy, efbPixel, &efb, bytes.data());
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
    // dot product) or one channel for every component, or colour; RGB565 reads back with alpha one.
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

    // THPGXYuv2RgbDraw: a decoded frame's planes (smg::decodeThpVideo) turned to RGB once, drawn over the EFB rectangle the
    // game's quad covers, its colour replacing what is there (no blending, depth or alpha written).
    void movieFrame(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, unsigned generation,
                    int x, int top, int w, int h) {
        MovieFrame& m = movie[generation & 1];
        const unsigned tw = textureSide(float(width)), th = textureSide(float(height));
        if (m.ready && (m.texture.width != tw || m.texture.height != th)) C3D_TexDelete(&m.texture), m.ready = false;
        if (!m.ready) {
            if (!C3D_TexInit(&m.texture, tw, th, GPU_RGB565)) stop("a movie frame with no memory for its texture");
            C3D_TexSetFilter(&m.texture, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetWrap(&m.texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            m.ready = true, m.key = nullptr;
        }
        if (!inFrame) begin();
        flushBatch();
        if (m.key != y || m.generation != generation) {
            // Rows go in upside down and in 8x8 tiles, as tile() lays every texture out for the PICA.
            auto* texels = static_cast<std::uint16_t*>(m.texture.data);
            for (unsigned iy = 0; iy < height; ++iy) {
                const unsigned row = th - 1 - iy;
                const std::uint8_t *ys = y + std::size_t(iy) * width, *us = u + std::size_t(iy / 2) * (width / 2), *vs = v + std::size_t(iy / 2) * (width / 2);
                std::uint16_t* tiles = texels + std::size_t(row / 8) * (tw / 8) * 64;
                const unsigned rowBits = (row & 1) << 1 | (row & 2) << 2 | (row & 4) << 3;
                for (unsigned ix = 0; ix < width; ++ix) {
                    std::uint8_t rgb[3];
                    thpPixel(ys[ix], us[ix / 2], vs[ix / 2], rgb);
                    const unsigned morton = (ix & 1) | (ix & 2) << 1 | (ix & 4) << 2 | rowBits;
                    tiles[(ix / 8) * 64 + morton] = std::uint16_t((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
                }
            }
            C3D_TexFlush(&m.texture);
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
        const GxDrawVertex a = corner(x, top, 0, 0), b = corner(x + w, top, s1, 0), c = corner(x + w, top + h, s1, t1), d = corner(x, top + h, 0, t1);
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
        ensureCpuBuffers();
        C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(used), static_cast<int>(n));
        used += n;
        applied = false;
        fresh[0] = fresh[1] = false;
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
        return 0xFFFFFF - (readEfb(true)[sx * 240 + (239 - sy)] & 0xFFFFFF);  // the port keeps nearer as larger
    }

    static u32 copyClearColor(const GxCommandProcessor& gpu) {
        const std::uint32_t ar = gpu.bp[0x4F], gb = gpu.bp[0x50];
        // citro3d's clear colour is 0xRRGGBBAA.
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
        flushBatch();
        if (dumping) std::fprintf(stderr, "gx dump: frame ends after %u draws\n", dumpIndex), dumping = false;
        C3D_FrameEnd(0);
        inFrame = false;
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
    LightLock_Unlock(&sChanged.lock);
}

void gxRenderMovieFrame(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, unsigned generation,
                        int x, int top, int w, int h) {
    const PortMemory memory;
    const Timed timed(3);
    renderer().movieFrame(y, u, v, width, height, generation, x, top, w, h);
}

std::uint32_t gxRenderPeek(const GxCommandProcessor& gpu, unsigned x, unsigned y, bool depth) {
    const PortMemory memory;
    return renderer().peek(gpu, x, y, depth);
}

void gxRenderCopy(const GxCommandProcessor& gpu, std::uint32_t command) {
    const PortMemory memory;
    {
        const Timed timed(command >> 14 & 1 ? 5 : 2);
        renderer().copy(gpu, command);
    }
    if (command >> 14 & 1) renderer().perfLine();
    // Every 300th frame the game's state is logged (source/compat/GameScene.cpp), from the thread that draws.
    static unsigned frames = 0;
    if (++frames % 300 == 0) smgHeartbeat();
}
} // namespace smg

// GameSystem::frameLoop's phases (adapted): 0 drawing begins, 1 the logic step, 2 animation, 3 the retrace wait. Ticks
// are read only here, a few times a frame: Azahar charges 150 cycles for each read.
extern "C" void smgFramePhase(int phase) {
    static u64 last = 0;
    static int current = -1;
    const u64 now = svcGetSystemTick();
    auto& perf = smg::renderer().perf;
    if (current >= 0) perf.phaseTicks[current] += now - last;
    if (phase == 0 && current >= 0) ++perf.phaseFrames;
    current = phase, last = now;
}
