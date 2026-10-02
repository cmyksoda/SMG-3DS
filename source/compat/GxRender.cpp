// The Wii GPU's drawing on the 3DS. Each primitive the command processor hands over becomes triangles transformed on
// the CPU (smg::buildGxDraw) and drawn with citro3d, the TEV program as combiner stages and the textures fetched from
// the Wii's memory. An EFB copy to the display ends the frame. What this can't draw yet stops by name.
#include <3ds.h>
#include <citro3d.h>
#include <GxShader.hpp>
#include <private/pe_reg.h>
#include <smg/FreeStore.hpp>
#include <smg/GxCopy.hpp>
#include <smg/GxDraw.hpp>
#include <smg/GxRender.hpp>
#include <smg/Pica.hpp>
#include <smg/Texture.hpp>
#include <smg/Thp.hpp>
#include <revolution/os.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

extern "C" void smgHeartbeat();  // source/compat/GameScene.cpp

namespace smg {
namespace {
constexpr std::size_t VertexBytes = 2u << 20, CommandBytes = 1u << 20;
// Room a draw's commands need at most, in words: when less is left, the frame is ended and begun again (see roomFor).
constexpr u32 DrawCommandWords = 0x800;
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
    std::size_t used = 0, flushed = 0;
    GxDraw drawing;  // kept between draws, so its vertices' memory is too
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
    GxFrame frame;
    bool indirectNoted = false;
    // Per-frame costs, logged every few seconds (perfLine): what each optimization is measured by.
    struct Perf {
        u64 since = 0, renderTicks = 0;
        u32 frames = 0, primitives = 0, vertices = 0, triangles = 0, draws = 0, sameState = 0, lastBpChanges = ~0u;
    } perf;

    void init() {
        C3D_Init(CommandBytes);
        target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DisplayTransferFlags);
        shader = DVLB_ParseFile(reinterpret_cast<u32*>(gxShader), sizeof gxShader);
        shaderProgramInit(&program);
        vertices = static_cast<GxDrawVertex*>(linearAlloc(VertexBytes));
        if (!target || !shader || R_FAILED(shaderProgramSetVsh(&program, &shader->DVLE[0])) || !vertices) stop("setting up citro3d");
        ready = true;
    }

    void begin() {
        if (!ready) init();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_FrameDrawOn(target);
        if (clearPending) C3D_RenderTargetClear(target, C3D_CLEAR_ALL, clearColor, clearDepth);
        fresh[0] = fresh[1] = false;
        clearPending = false;
        used = flushed = 0;
        inFrame = true;
    }

    // The vertices written since the GPU last took a command list, out of the CPU's cache (Azahar has none).
    void flushVertices() {
        if (used > flushed) GSPGPU_FlushDataCache(vertices + flushed, (used - flushed) * sizeof(GxDrawVertex));
        flushed = used;
    }

    // Ends the frame drawn so far without showing it; resume() begins again on the same target once the GPU is done, so
    // the vertex buffer can start over. Inside a frame citro3d's "sync" transfers only queue: readbacks go between.
    void pause() {
        C3D_RenderTargetDetachOutput(target);
        C3D_FrameEnd(0);
    }
    void resume() {
        C3D_FrameBegin(0);
        C3D_FrameDrawOn(target);
        C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DisplayTransferFlags);
        used = flushed = 0;
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
    void depthFromTexture(GxDraw& draw) {
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
        const float depth01 = static_cast<float>((depth + draw.depthTextureOffset) & 0xFFFFFF) / 16777215.0f;
        for (GxDrawVertex& v : draw.triangles) v.position[2] = (depth01 - 1) * v.position[3];
    }

    void draw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* data) {
        ++perf.primitives, perf.vertices += count;
        if (gpu.bpChanges == perf.lastBpChanges) ++perf.sameState;
        perf.lastBpChanges = gpu.bpChanges;
        GxDraw& d = drawing;
        buildGxDraw(gpu, kind, format, count, data, frame, d);
        if (d.unsupported) stop(d.unsupported);
        if (d.material.blend.type == 2 && d.material.blend.logic != 3) stop("a logic op other than copy");
        if (d.depthTextureOp) depthFromTexture(d);
        if (d.indirectDropped && !indirectNoted) {
            std::fprintf(stderr, "The Wii GPU's drawing: indirect texture offsets are left out (the PICA has no dependent reads)\n");
            indirectNoted = true;
        }
        // GX's scissor in screen pixels, then on the target turned a quarter as the LCD is (citro3d's convention).
        const int x0 = std::clamp(static_cast<int>(std::lround(d.scissor[0] * 400.0f / frame.width)), 0, 400);
        const int x1 = std::clamp(static_cast<int>(std::lround(d.scissor[2] * 400.0f / frame.width)), 0, 400);
        const int y0 = std::clamp(static_cast<int>(std::lround(d.scissor[1] * 240.0f / frame.height)), 0, 240);
        const int y1 = std::clamp(static_cast<int>(std::lround(d.scissor[3] * 240.0f / frame.height)), 0, 240);
        if (d.triangles.empty() || x1 <= x0 || y1 <= y0) return;
        if (!inFrame) begin();
        roomFor(d.triangles.size());
        if ((used + d.triangles.size()) * sizeof(GxDrawVertex) > VertexBytes) stop("a frame with more vertices than the port's buffer");
        GxDrawVertex* out = vertices + used;
        for (std::size_t i = 0; i < d.triangles.size(); ++i) {
            out[i] = d.triangles[i];
            // The top screen's framebuffer is turned a quarter: x takes y, y takes minus x, as citro3d's tilted projections.
            out[i].position[0] = d.triangles[i].position[1];
            out[i].position[1] = -d.triangles[i].position[0];
        }

        C3D_BindProgram(&program);
        C3D_AttrInfo* attributes = C3D_GetAttrInfo();
        AttrInfo_Init(attributes);
        AttrInfo_AddLoader(attributes, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(attributes, 1, GPU_UNSIGNED_BYTE, 4);
        for (int unit = 0; unit < 3; ++unit) AttrInfo_AddLoader(attributes, 2 + unit, GPU_FLOAT, 2);
        C3D_BufInfo* buffers = C3D_GetBufInfo();
        BufInfo_Init(buffers);
        BufInfo_Add(buffers, out, sizeof(GxDrawVertex), 5, 0x43210);
        const PicaPixel pixel = preparePixel(d.material, d.program);
        pixel.apply();
        const int mask = (d.colorUpdate ? GPU_WRITE_COLOR & ~GPU_WRITE_ALPHA : 0) | (d.alphaUpdate ? GPU_WRITE_ALPHA : 0) |
                         (pixel.depthWrite ? GPU_WRITE_DEPTH : 0);
        C3D_DepthTest(pixel.depthTest, pixel.depthTest ? pixel.depthFunc : GPU_ALWAYS, static_cast<GPU_WRITEMASK>(mask));
        C3D_CullFace(GPU_CULL_NONE);  // culled in EFB space already
        if (x0 == 0 && y0 == 0 && x1 == 400 && y1 == 240) C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        else C3D_SetScissor(GPU_SCISSOR_NORMAL, 240 - y1, 400 - x1, 240 - y0, 400 - x0);
        for (unsigned unit = 0; unit < d.program.unitCount; ++unit) C3D_TexBind(unit, &texture(d.textures[d.program.units[unit].texture]));
        C3D_DrawArrays(GPU_TRIANGLES, 0, static_cast<int>(d.triangles.size()));
        fresh[0] = fresh[1] = false;
        used += d.triangles.size();
        ++perf.draws, perf.triangles += d.triangles.size() / 3;
    }

    void perfLine() {
        const u64 now = svcGetSystemTick();
        ++perf.frames;
        if (!perf.since) perf.since = now;
        const double ms = double(now - perf.since) * 1000.0 / SYSCLOCK_ARM11;
        if (ms < 4000) return;
        const double n = perf.frames;
        std::fprintf(stderr, "gx perf: %lu frames, %.1f ms/frame, render path %.1f ms/frame; per frame %.0f primitives (%.0f same state), "
                     "%.0f vertices, %.0f triangles, %.0f draws\n", static_cast<unsigned long>(perf.frames), ms / n,
                     double(perf.renderTicks) * 1000.0 / SYSCLOCK_ARM11 / n, perf.primitives / n, perf.sameState / n, perf.vertices / n,
                     perf.triangles / n, perf.draws / n);
        const u32 last = perf.lastBpChanges;
        perf = Perf{};
        perf.since = now, perf.lastBpChanges = last;
    }

    // A Wii frame's commands can outgrow citro3d's buffer (or its vertices the port's): the frame is ended and begun
    // again on the same target with its display output held back, so no part-drawn frame is shown.
    void roomFor(std::size_t vertexCount) {
        u32* buffer;
        u32 size, offset;
        GPUCMD_GetBuffer(&buffer, &size, &offset);
        if (size - offset >= DrawCommandWords && (used + vertexCount) * sizeof(GxDrawVertex) <= VertexBytes) return;
        flushVertices();
        pause();
        resume();
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
        GxTextureCopy copy;
        if (const char* why = readTextureCopy(gpu, command, copy)) stop(why);
        if (!inFrame) begin();
        if (!sCopiesStayOnGpu || !gpuCopy(gpu, copy)) {
            dropGpuCopy(copy.address);
            const Efb efb = efbView(gpu);
            encodeTextureCopy(copy, efbPixel, &efb, static_cast<std::uint8_t*>(OSPhysicalToCached(copy.address)));
        }
        forget(copy.address, textureCopySize(copy));
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
        flushVertices();
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
        C3D_BindProgram(&program);
        C3D_AttrInfo* attributes = C3D_GetAttrInfo();
        AttrInfo_Init(attributes);
        AttrInfo_AddLoader(attributes, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(attributes, 1, GPU_UNSIGNED_BYTE, 4);
        for (int unit = 0; unit < 3; ++unit) AttrInfo_AddLoader(attributes, 2 + unit, GPU_FLOAT, 2);
        C3D_BufInfo* buffers = C3D_GetBufInfo();
        BufInfo_Init(buffers);
        BufInfo_Add(buffers, out, sizeof(GxDrawVertex), 5, 0x43210);
        copyCombiner(copy, taps, weights, noAlpha);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        for (int unit = 0; unit < 3; ++unit) C3D_TexBind(unit, &scratch);
        C3D_DrawArrays(GPU_TRIANGLES, 0, static_cast<int>(n));
        used += n;
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
        flushVertices(), pause(), resume();  // the GPU has drawn the copy's quad
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
        C3D_BindProgram(&program);
        C3D_AttrInfo* attributes = C3D_GetAttrInfo();
        AttrInfo_Init(attributes);
        AttrInfo_AddLoader(attributes, 0, GPU_FLOAT, 4);
        AttrInfo_AddLoader(attributes, 1, GPU_UNSIGNED_BYTE, 4);
        for (int unit = 0; unit < 3; ++unit) AttrInfo_AddLoader(attributes, 2 + unit, GPU_FLOAT, 2);
        C3D_BufInfo* buffers = C3D_GetBufInfo();
        BufInfo_Init(buffers);
        BufInfo_Add(buffers, out, sizeof(GxDrawVertex), 5, 0x43210);
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
        C3D_DrawArrays(GPU_TRIANGLES, 0, static_cast<int>(n));
        used += n;
        fresh[0] = fresh[1] = false;
    }

    // What has been drawn so far, read back from the target once the GPU has finished it; depth (D24S8) moves as raw
    // four-byte pixels too.
    const u32* readEfb(bool depth) {
        u32*& into = readback[depth];
        if (!into) into = static_cast<u32*>(linearAlloc(240 * 400 * 4));
        if (!into) stop("reading the EFB back with no memory to read it into");
        if (!fresh[depth]) {
            if (inFrame) flushVertices(), pause();
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
        if (!(command >> 14 & 1)) return textureCopy(gpu, command);
        const std::uint32_t size = gpu.bp[0x4A];
        frame.width = static_cast<float>((size & 0x3FF) + 1), frame.height = static_cast<float>((size >> 10 & 0x3FF) + 1);
        if (!inFrame) begin();
        C3D_FrameEnd(0);
        inFrame = false;
        if (command >> 11 & 1) clearColor = copyClearColor(gpu), clearDepth = copyClearDepth(gpu), clearPending = true;
    }
};

Renderer& renderer() {
    static Renderer instance;
    return instance;
}
} // namespace

void gxRenderPrimitive(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices) {
    const PortMemory memory;
    const u64 start = svcGetSystemTick();
    renderer().draw(gpu, kind, format, count, vertices);
    renderer().perf.renderTicks += svcGetSystemTick() - start;
}

void gxRenderCopiesStayOnGpu(bool stay) { sCopiesStayOnGpu = stay; }

void gxRenderMovieFrame(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, unsigned generation,
                        int x, int top, int w, int h) {
    const PortMemory memory;
    const u64 start = svcGetSystemTick();
    renderer().movieFrame(y, u, v, width, height, generation, x, top, w, h);
    renderer().perf.renderTicks += svcGetSystemTick() - start;
}

std::uint32_t gxRenderPeek(const GxCommandProcessor& gpu, unsigned x, unsigned y, bool depth) {
    const PortMemory memory;
    return renderer().peek(gpu, x, y, depth);
}

void gxRenderCopy(const GxCommandProcessor& gpu, std::uint32_t command) {
    const PortMemory memory;
    const u64 start = svcGetSystemTick();
    renderer().copy(gpu, command);
    renderer().perf.renderTicks += svcGetSystemTick() - start;
    if (command >> 14 & 1) renderer().perfLine();
    // Every 300th frame the game's state is logged (source/compat/GameScene.cpp), from the thread that draws.
    static unsigned frames = 0;
    if (++frames % 300 == 0) smgHeartbeat();
}
} // namespace smg
