#include <smg/GxCopy.hpp>
#include <smg/Texture.hpp>
#include <private/pe_reg.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::printf("FAIL: %s\n", message); std::exit(1); }
}

// The registers as GXSetTexCopySrc, GXSetTexCopyDst, GXSetCopyFilter and GXCopyTex leave them; returns PE_COPY_CMD.
struct Copy {
    unsigned x = 0, y = 0, width = 16, height = 8, format = 7, ccv = 2, rowBytes = 0;
    bool half = false, clear = false, clampTop = false, clampBottom = false;
    unsigned taps[7] = {0, 0, 21, 22, 21, 0, 0};  // the SDK's "no vertical filter": six-bit taps can't pass rows through
};
std::uint32_t issue(smg::GxCommandProcessor& gpu, const Copy& c, std::uint32_t address) {
    std::uint32_t source = 0, size = 0, base = 0, stride = 0, f0 = 0, f1 = 0, command = 0;
    PE_COPY_SRC_ADDR_SET_X(source, c.x); PE_COPY_SRC_ADDR_SET_Y(source, c.y);
    PE_COPY_SRC_SIZE_SET_X(size, c.width - 1); PE_COPY_SRC_SIZE_SET_Y(size, c.height - 1);
    PE_COPY_DST_BASE_SET_BASE(base, address >> 5);
    PE_COPY_DST_STRIDE_SET_STRIDE(stride, c.rowBytes >> 5);
    PE_COPY_VFILTER0_SET_COEFF0(f0, c.taps[0]); PE_COPY_VFILTER0_SET_COEFF1(f0, c.taps[1]);
    PE_COPY_VFILTER0_SET_COEFF2(f0, c.taps[2]); PE_COPY_VFILTER0_SET_COEFF3(f0, c.taps[3]);
    PE_COPY_VFILTER1_SET_COEFF4(f1, c.taps[4]); PE_COPY_VFILTER1_SET_COEFF5(f1, c.taps[5]);
    PE_COPY_VFILTER1_SET_COEFF6(f1, c.taps[6]);
    PE_COPY_CMD_SET_TEX_FORMAT(command, c.format & 7); PE_COPY_CMD_SET_TEX_FORMATH(command, c.format >> 3);
    PE_COPY_CMD_SET_CCV(command, c.ccv); PE_COPY_CMD_SET_MIP_MAP_FILTER(command, c.half);
    PE_COPY_CMD_SET_CLEAR(command, c.clear);
    PE_COPY_CMD_SET_CLAMP_TOP(command, c.clampTop); PE_COPY_CMD_SET_CLAMP_BOTTOM(command, c.clampBottom);
    gpu.bp[0x49] = source, gpu.bp[0x4A] = size, gpu.bp[0x4B] = base, gpu.bp[0x4D] = stride, gpu.bp[0x53] = f0, gpu.bp[0x54] = f1;
    return command;
}

// A 64x64 EFB whose every channel says where it is.
using Pixel = std::array<std::uint8_t, 4>;
Pixel pattern(const void*, int x, int y) {
    x = x < 0 ? 0 : x > 63 ? 63 : x, y = y < 0 ? 0 : y > 63 ? 63 : y;
    return {static_cast<std::uint8_t>(x * 4), static_cast<std::uint8_t>(y * 4), static_cast<std::uint8_t>((x ^ y) * 4),
            static_cast<std::uint8_t>(x + 2 * y)};
}
// What the SDK's three taps make of an EFB pixel's neighbourhood, for checking texels against.
Pixel filtered(smg::EfbPixel pixel, int x, int y) {
    Pixel out;
    for (int c = 0; c < 4; ++c) {
        const unsigned sum = 21 * pixel(nullptr, x, y - 1)[c] + 22 * pixel(nullptr, x, y)[c] + 21 * pixel(nullptr, x, y + 1)[c];
        out[c] = static_cast<std::uint8_t>(sum > 255 * 64 ? 255 : sum >> 6);
    }
    return out;
}
Pixel white(const void*, int, int) { return {255, 255, 255, 255}; }
Pixel row3(const void*, int, int y) { return {0, 0, 0, static_cast<std::uint8_t>(y == 3 ? 255 : 0)}; }
Pixel checker(const void*, int x, int y) { return {0, 0, 0, static_cast<std::uint8_t>((x & 1) * 100 + (y & 1) * 50)}; }

std::vector<std::uint8_t> make(smg::GxCommandProcessor& gpu, const Copy& c, smg::EfbPixel pixel, smg::GxTextureCopy& copy) {
    const char* why = smg::readTextureCopy(gpu, issue(gpu, c, 0x1000), copy);
    check(!why, why ? why : "a texture copy reads");
    std::vector<std::uint8_t> out(smg::textureCopySize(copy), 0xEE);
    smg::encodeTextureCopy(copy, pixel, nullptr, out.data());
    return out;
}

void readsTheSdkCopy() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});  // more than a 3DS thread's stack
    smg::GxCommandProcessor& gpu = *owned;
    Copy c;
    c.width = 640, c.height = 456, c.rowBytes = 80 * 32;  // GX_CTF_A8
    smg::GxTextureCopy copy;
    check(!smg::readTextureCopy(gpu, issue(gpu, c, 0x8000), copy), "the screen's alpha capture reads");
    check(copy.width == 640 && copy.height == 456 && copy.texWidth == 640 && copy.format == 7 && !copy.intensity && copy.address == 0x8000,
          "its rectangle, format and destination are the SDK's");
    check(copy.filter[2] == 21 && copy.filter[3] == 22 && copy.filter[4] == 21 && !copy.filter[0], "and its filter's taps");
    check(smg::textureCopySize(copy) == 640 * 456, "an A8 copy fills whole 8x4 tiles");
}

void a8Tiles() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});
    smg::GxCommandProcessor& gpu = *owned;
    Copy c;
    c.x = 8, c.y = 4, c.rowBytes = 2 * 32;
    smg::GxTextureCopy copy;
    const auto out = make(gpu, c, pattern, copy);
    bool ok = out.size() == 2 * 2 * 32;
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 16; ++x)
            ok = ok && out[(y / 4) * 64 + (x / 8) * 32 + (y % 4) * 8 + x % 8] == filtered(pattern, int(8 + x), int(4 + y))[3];
    check(ok, "an A8 copy writes each texel's alpha in 8x4 tiles");
}

void intensity() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});
    smg::GxCommandProcessor& gpu = *owned;
    Copy c;
    c.format = 1, c.ccv = 3, c.rowBytes = 64;
    smg::GxTextureCopy copy;
    auto out = make(gpu, c, white, copy);
    check(copy.intensity && out[0] == 235 && out[127] == 235, "an I8 copy of white is the pixel engine's Y, 235");
    c.ccv = 2;
    out = make(gpu, c, white, copy);
    check(!copy.intensity && out[0] == 255, "an R8 copy keeps red as it is");
}

void filters() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});
    smg::GxCommandProcessor& gpu = *owned;
    Copy c;
    c.y = 4, c.rowBytes = 64;
    smg::GxTextureCopy copy;
    auto out = make(gpu, c, row3, copy);
    check(out[0] == (21 * 255) >> 6 && out[8] == 0, "the SDK's unfiltered copy still blends the row above its first");
    c.clampTop = true;
    out = make(gpu, c, row3, copy);
    check(out[0] == 0, "unless it clamps at the top");
    c = Copy{};
    c.rowBytes = 32, c.half = true, c.width = 16;
    out = make(gpu, c, checker, copy);
    check(copy.texWidth == 8 && copy.texHeight == 4 && out[0] == 75 && out[31] == 75, "the box filter averages each 2x2");
}

void roundTrips() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});
    smg::GxCommandProcessor& gpu = *owned;
    const struct { unsigned format; smg::GxFormat texture; unsigned rowBytes; } cases[] = {
        {6, smg::GxFormat::RGBA8, 4 * 64}, {4, smg::GxFormat::RGB565, 4 * 32}, {5, smg::GxFormat::RGB5A3, 4 * 32},
        {3, smg::GxFormat::IA8, 4 * 32}, {0, smg::GxFormat::I4, 2 * 32}};
    for (const auto& k : cases) {
        Copy c;
        c.format = k.format, c.rowBytes = k.rowBytes;
        smg::GxTextureCopy copy;
        const auto out = make(gpu, c, pattern, copy);
        smg::Image image;
        std::string error;
        check(smg::decodeGxImage({out.data(), out.size()}, k.texture, 16, 8, {}, smg::GxPalette::IA8, image, error), "a copied texture decodes");
        bool ok = true;
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 16; ++x) {
                const Pixel p = filtered(pattern, int(x), int(y));
                const std::uint32_t got = image.pixels[y * 16 + x];
                const int r = got >> 24, g = got >> 16 & 0xFF, b = got >> 8 & 0xFF, a = got & 0xFF;
                const auto close = [](int v, int want, int bits) { return std::abs(v - want) <= (256 >> bits); };
                switch (k.format) {
                case 6: ok = ok && r == p[0] && g == p[1] && b == p[2] && a == p[3]; break;
                case 4: ok = ok && close(r, p[0], 5) && close(g, p[1], 6) && close(b, p[2], 5) && a == 255; break;
                case 5: ok = ok && close(r, p[0], 4) && close(g, p[1], 4) && close(b, p[2], 4) && close(a, p[3], 3); break;
                case 3: ok = ok && r == p[0] && a == p[3]; break;
                case 0: ok = ok && close(r, p[0], 4); break;
                }
            }
        check(ok, "and reads back as the EFB's pixels, to the format's precision");
    }
}

Pixel solid(const void*, int, int) { return {10, 20, 30, 40}; }

// RG8 and GB8 copies keep two channels a texel, the second named first (Dolphin's encoder writes them so).
void twoChannels() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});
    smg::GxCommandProcessor& gpu = *owned;
    Copy c;
    c.rowBytes = 128;  // four 4x4 tiles of two-byte texels
    smg::GxTextureCopy copy;
    c.format = 11;
    auto out = make(gpu, c, solid, copy);
    check(out[0] == 20 && out[1] == 10 && out[30] == 20 && out[31] == 10, "an RG8 copy writes green, then red");
    c.format = 12;
    out = make(gpu, c, solid, copy);
    check(out[0] == 30 && out[1] == 20 && smg::textureCopySize(copy) == 2 * 128, "a GB8 copy writes blue, then green, in 4x4 tiles");
}

// A depth copy writes z's bytes as each Z format keeps them, unfiltered; halved, it averages z as one number.
std::uint32_t depthAt(int x, int y) { return 0x123456u + std::uint32_t(x) * 0x010203u + std::uint32_t(y) * 0x000101u; }
Pixel depthPattern(const void*, int x, int y) {
    const std::uint32_t z = depthAt(x < 0 ? 0 : x, y < 0 ? 0 : y);
    return {static_cast<std::uint8_t>(z >> 16), static_cast<std::uint8_t>(z >> 8), static_cast<std::uint8_t>(z), 255};
}
Pixel depthSeam(const void*, int x, int) { return x & 1 ? Pixel{0x01, 0x00, 0x00, 255} : Pixel{0x00, 0xFF, 0x00, 255}; }
void depthCopies() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});
    smg::GxCommandProcessor& gpu = *owned;
    gpu.bp[0x43] = 3;
    smg::GxTextureCopy copy;
    Copy c;
    c.rowBytes = 64;
    c.format = 1;  // Z8, 8x4 tiles of a byte
    auto out = make(gpu, c, depthPattern, copy);
    check(copy.depth && out[0] == (depthAt(0, 0) >> 16 & 0xFF) && out[9] == (depthAt(1, 1) >> 16 & 0xFF), "a Z8 copy keeps z's top byte, unfiltered");
    c.format = 9;
    out = make(gpu, c, depthPattern, copy);
    check(out[9] == (depthAt(1, 1) >> 8 & 0xFF), "a Z8M copy its middle byte");
    c.format = 10;
    out = make(gpu, c, depthPattern, copy);
    check(out[9] == (depthAt(1, 1) & 0xFF), "a Z8L copy its low byte");
    c.format = 3, c.rowBytes = 128;  // Z16, 4x4 tiles of two bytes
    out = make(gpu, c, depthPattern, copy);
    check(out[10] == (depthAt(1, 1) >> 16 & 0xFF) && out[11] == (depthAt(1, 1) >> 8 & 0xFF), "a Z16 copy its top two, big-endian");
    c.format = 12;
    out = make(gpu, c, depthPattern, copy);
    check(out[10] == (depthAt(1, 1) >> 8 & 0xFF) && out[11] == (depthAt(1, 1) & 0xFF), "a Z16L copy its low two");
    c.format = 6, c.rowBytes = 256;  // Z24X8: an AR block, then a GB block
    out = make(gpu, c, depthPattern, copy);
    check(out[10] == 255 && out[11] == (depthAt(1, 1) >> 16 & 0xFF) && out[42] == (depthAt(1, 1) >> 8 & 0xFF) && out[43] == (depthAt(1, 1) & 0xFF),
          "a Z24X8 copy all of z, in RGBA8's two blocks");
    c.format = 3, c.rowBytes = 64, c.half = true;
    out = make(gpu, c, depthSeam, copy);
    check(out[0] == 0x00 && out[1] == 0xFF, "halved, z's bytes average as one number (0x00FF00 and 0x010000 make 0x00FF80)");
}

void refusals() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(smg::GxCommandProcessor::Host{});
    smg::GxCommandProcessor& gpu = *owned;
    smg::GxTextureCopy copy;
    Copy c;
    c.rowBytes = 64;
    std::uint32_t command = issue(gpu, c, 0x1000);
    gpu.bp[0x43] = 3;  // PE_CONTROL's pixel type: Z
    c.format = 4;
    check(smg::readTextureCopy(gpu, issue(gpu, c, 0x1000), copy), "a depth copy to a colour format is refused by name");
    c.format = 7;
    gpu.bp[0x43] = 0;
    (void)command;
    c.format = 13;
    check(smg::readTextureCopy(gpu, issue(gpu, c, 0x1000), copy), "so is a copy to a format the pixel engine has no encoder for");
    c.format = 7, c.rowBytes = 32;
    check(smg::readTextureCopy(gpu, issue(gpu, c, 0x1000), copy), "and one whose rows of tiles overlap");
}
} // namespace

int runGxCopyTests() {
    readsTheSdkCopy();
    a8Tiles();
    intensity();
    filters();
    roundTrips();
    twoChannels();
    depthCopies();
    refusals();
    std::printf("%u GX copy checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGxCopyTests(); }
#endif
