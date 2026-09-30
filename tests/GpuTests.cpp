#include <smg/Etc1.hpp>
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::printf("FAIL: %s\n", message); std::exit(1); }
}

constexpr unsigned Size = 64;

unsigned morton(unsigned x, unsigned y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

smg::Image pattern(bool alpha) {
    smg::Image image;
    image.width = image.height = Size;
    for (unsigned y = 0; y < Size; ++y) for (unsigned x = 0; x < Size; ++x) {
        const unsigned r = x * 4, g = y * 4, b = ((x / 8 + y / 8) & 1) ? 220 : 30;
        image.pixels.push_back(r << 24 | g << 16 | b << 8 | (alpha ? (x + y) * 2 : 255));
    }
    return image;
}

bool uploadRgba(C3D_Tex& texture, const smg::Image& image) {
    if (!C3D_TexInit(&texture, Size, Size, GPU_RGBA8)) return false;
    auto* texels = static_cast<std::uint32_t*>(texture.data);
    for (unsigned y = 0; y < Size; ++y) for (unsigned x = 0; x < Size; ++x)
        texels[((y / 8) * (Size / 8) + x / 8) * 64 + morton(x, y)] = image.pixels[y * Size + x];
    C3D_TexFlush(&texture);
    return true;
}

bool uploadEtc1(C3D_Tex& texture, const std::vector<std::uint8_t>& bytes, bool alpha) {
    if (!C3D_TexInit(&texture, Size, Size, alpha ? GPU_ETC1A4 : GPU_ETC1)) return false;
    std::memcpy(texture.data, bytes.data(), std::min<std::size_t>(texture.size, bytes.size()));
    C3D_TexFlush(&texture);
    return true;
}

// Draws the texture 1:1 with nearest sampling and copies the finished frame out, untiled.
void render(C3D_RenderTarget* target, C3D_Tex& texture, std::uint32_t* out) {
    C3D_TexSetFilter(&texture, GPU_NEAREST, GPU_NEAREST);
    const Tex3DS_SubTexture sub{Size, Size, 0.0f, 1.0f, 1.0f, 0.0f};
    const C2D_Image image{&texture, &sub};
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(target);
    C2D_DrawImageAt(image, 0, 0, 0);
    C3D_FrameEnd(0);
    C3D_SyncDisplayTransfer(static_cast<u32*>(target->frameBuf.colorBuf), GX_BUFFER_DIM(Size, Size), out,
        GX_BUFFER_DIM(Size, Size), GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
        GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

unsigned compare(C3D_RenderTarget* target, bool alpha) {
    const auto source = pattern(alpha);
    const auto bytes = smg::encodeEtc1(source, alpha);
    const auto decoded = smg::decodeEtc1(bytes.data(), Size, Size, alpha);
    C3D_Tex etc1, reference;
    check(uploadEtc1(etc1, bytes, alpha) && uploadRgba(reference, decoded), "texture allocation");
    auto* a = static_cast<std::uint32_t*>(linearAlloc(Size * Size * 4));
    auto* b = static_cast<std::uint32_t*>(linearAlloc(Size * Size * 4));
    check(a && b, "readback allocation");
    render(target, etc1, a);
    render(target, reference, b);
    unsigned worst = 0;
    for (unsigned i = 0; i < Size * Size; ++i)
        for (int shift = 0; shift < 32; shift += 8) {
            const int d = std::abs(int(a[i] >> shift & 255) - int(b[i] >> shift & 255));
            if (unsigned(d) > worst) worst = d;
        }
    linearFree(a);
    linearFree(b);
    C3D_TexDelete(&etc1);
    C3D_TexDelete(&reference);
    return worst;
}
} // namespace

int runGpuTests() {
    check(C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) && C2D_Init(C2D_DEFAULT_MAX_OBJECTS), "citro3d and citro2d start");
    C2D_Prepare();
    C3D_RenderTarget* target = C3D_RenderTargetCreate(Size, Size, GPU_RB_RGBA8, -1);
    check(target != nullptr, "offscreen target");
    // The GPU's own ETC1 decoding must agree with ours pixel for pixel, or the layout is wrong.
    const unsigned opaque = compare(target, false);
    check(opaque <= 1, "GPU decodes the ETC1 layout as encoded");
    const unsigned translucent = compare(target, true);
    check(translucent <= 1, "GPU decodes the ETC1A4 layout as encoded");
    C3D_RenderTargetDelete(target);
    C2D_Fini();
    C3D_Fini();
    std::printf("%u GPU texture checks passed (worst difference %u, %u)\n", checks, opaque, translucent);
    return 0;
}
