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
    std::size_t used = 0;
    // The target's colour and depth read back (both four bytes a pixel, the screen's way round), until drawn on again.
    u32* readback[2] = {};
    bool fresh[2] = {};
    // A texture by where and how GX fetches it.
    using Key = std::tuple<u32, u16, u16, u8, u8, u8, u8, u8>;
    std::map<Key, C3D_Tex> textures;
    GxFrame frame;
    bool indirectNoted = false;

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
        used = 0;
        inFrame = true;
    }

    C3D_Tex& texture(const GxTextureMap& map) {
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
        GxDraw d;
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
        // The GPU reads this memory itself: on hardware the CPU's cache must hand it over (Azahar has no cache).
        GSPGPU_FlushDataCache(out, d.triangles.size() * sizeof(GxDrawVertex));

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
    }

    // A Wii frame's commands can outgrow citro3d's buffer (or its vertices the port's): the frame is ended and begun
    // again on the same target with its display output held back, so no part-drawn frame is shown.
    void roomFor(std::size_t vertexCount) {
        u32* buffer;
        u32 size, offset;
        GPUCMD_GetBuffer(&buffer, &size, &offset);
        if (size - offset >= DrawCommandWords && (used + vertexCount) * sizeof(GxDrawVertex) <= VertexBytes) return;
        C3D_RenderTargetDetachOutput(target);
        C3D_FrameEnd(0);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // waits for the GPU, so the vertex buffer can start over
        C3D_FrameDrawOn(target);
        C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DisplayTransferFlags);
        used = 0;
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
        const Efb efb{readEfb(false), 400.0f / frame.width, 240.0f / frame.height, PE_CONTROL_GET_PIXTYPE(gpu.bp[0x43])};
        encodeTextureCopy(copy, efbPixel, &efb, static_cast<std::uint8_t*>(OSPhysicalToCached(copy.address)));
        forget(copy.address, textureCopySize(copy));
        if (copy.clear) C3D_RenderTargetClear(target, C3D_CLEAR_ALL, copyClearColor(gpu), copyClearDepth(gpu)), fresh[0] = fresh[1] = false;
    }

    // What has been drawn so far goes to the GPU and is finished before the target is read; depth (D24S8) moves as
    // raw four-byte pixels too.
    const u32* readEfb(bool depth) {
        u32*& into = readback[depth];
        if (!into) into = static_cast<u32*>(linearAlloc(240 * 400 * 4));
        if (!into) stop("reading the EFB back with no memory to read it into");
        if (!fresh[depth]) {
            if (inFrame) C3D_FrameSplit(0);
            void* buffer = depth ? target->frameBuf.depthBuf : target->frameBuf.colorBuf;
            C3D_SyncDisplayTransfer(static_cast<u32*>(buffer), GX_BUFFER_DIM(240, 400), into, GX_BUFFER_DIM(240, 400), ReadbackFlags);
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
    renderer().draw(gpu, kind, format, count, vertices);
}

std::uint32_t gxRenderPeek(const GxCommandProcessor& gpu, unsigned x, unsigned y, bool depth) {
    const PortMemory memory;
    return renderer().peek(gpu, x, y, depth);
}

void gxRenderCopy(const GxCommandProcessor& gpu, std::uint32_t command) {
    const PortMemory memory;
    renderer().copy(gpu, command);
    // Every 300th frame the game's state is logged (source/compat/GameScene.cpp), from the thread that draws.
    static unsigned frames = 0;
    if (++frames % 300 == 0) smgHeartbeat();
}
} // namespace smg
