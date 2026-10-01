// The Wii GPU's drawing on the 3DS. Each primitive the command processor hands over becomes triangles transformed on
// the CPU (smg::buildGxDraw) and drawn with citro3d, the TEV program as combiner stages and the textures fetched from
// the Wii's memory. An EFB copy to the display ends the frame. What this can't draw yet stops by name.
#include <3ds.h>
#include <citro3d.h>
#include <GxShader.hpp>
#include <smg/FreeStore.hpp>
#include <smg/GxDraw.hpp>
#include <smg/GxRender.hpp>
#include <smg/Pica.hpp>
#include <smg/Texture.hpp>
#include <revolution/os.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

extern "C" void smgHeartbeat();  // source/compat/GameScene.cpp

namespace smg {
namespace {
constexpr std::size_t VertexBytes = 2u << 20;
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
    // A texture by where and how GX fetches it.
    using Key = std::tuple<u32, u16, u16, u8, u8, u8, u8, u8>;
    std::map<Key, C3D_Tex> textures;
    GxFrame frame;
    bool scissorNoted = false;

    void init() {
        C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
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
        if (!scissorNoted && (d.scissor[0] > 0 || d.scissor[1] > 0 || d.scissor[2] < frame.width || d.scissor[3] < frame.height)) {
            std::fprintf(stderr, "The Wii GPU's drawing: a scissor inside the EFB is drawn unscissored for now\n");
            scissorNoted = true;
        }
        if (d.triangles.empty()) return;
        if (!inFrame) begin();
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
        for (unsigned unit = 0; unit < d.program.unitCount; ++unit) C3D_TexBind(unit, &texture(d.textures[d.program.units[unit].texture]));
        C3D_DrawArrays(GPU_TRIANGLES, 0, static_cast<int>(d.triangles.size()));
        used += d.triangles.size();
    }

    // PE_COPY_CMD: opcode 1 copies to the display, 0 to a texture; bit 11 clears the EFB after.
    void copy(const GxCommandProcessor& gpu, std::uint32_t command) {
        if (!(command >> 14 & 1)) stop("an EFB copy to a texture");
        const std::uint32_t size = gpu.bp[0x4A];
        frame.width = static_cast<float>((size & 0x3FF) + 1), frame.height = static_cast<float>((size >> 10 & 0x3FF) + 1);
        if (!inFrame) begin();
        C3D_FrameEnd(0);
        inFrame = false;
        if (command >> 11 & 1) {
            const std::uint32_t ar = gpu.bp[0x4F], gb = gpu.bp[0x50];
            // citro3d's clear colour is 0xRRGGBBAA; its depth buffer keeps nearer as larger.
            clearColor = (ar & 0xFF) << 24 | (gb >> 8 & 0xFF) << 16 | (gb & 0xFF) << 8 | (ar >> 8 & 0xFF);
            clearDepth = 0xFFFFFF - (gpu.bp[0x51] & 0xFFFFFF);
            clearPending = true;
        }
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

void gxRenderCopy(const GxCommandProcessor& gpu, std::uint32_t command) {
    const PortMemory memory;
    renderer().copy(gpu, command);
    // Every 300th frame the game's state is logged (source/compat/GameScene.cpp), from the thread that draws.
    static unsigned frames = 0;
    if (++frames % 300 == 0) smgHeartbeat();
}
} // namespace smg
