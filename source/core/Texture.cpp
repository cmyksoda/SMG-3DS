#include <smg/Texture.hpp>

namespace smg {
namespace {
struct Layout { unsigned blockWidth, blockHeight, blockBytes; };

bool layout(GxFormat format, Layout& out) {
    switch (format) {
    case GxFormat::I4: case GxFormat::C4: case GxFormat::CMPR: out = {8, 8, 32}; return true;
    case GxFormat::I8: case GxFormat::IA4: case GxFormat::C8: out = {8, 4, 32}; return true;
    case GxFormat::IA8: case GxFormat::RGB565: case GxFormat::RGB5A3: case GxFormat::C14X2: out = {4, 4, 32}; return true;
    case GxFormat::RGBA8: out = {4, 4, 64}; return true;
    }
    return false;
}

bool fail(std::string& error, const char* text) { error = text; return false; }
unsigned be16(const std::uint8_t* p) { return (p[0] << 8) | p[1]; }
std::uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a) { return (r << 24) | (g << 16) | (b << 8) | a; }
unsigned expand5(unsigned v) { return (v << 3) | (v >> 2); }
unsigned expand4(unsigned v) { return v * 17; }

std::uint32_t rgb565(unsigned v) { return rgba(expand5(v >> 11), (((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4), expand5(v & 31), 255); }
std::uint32_t rgb5a3(unsigned v) {
    if (v & 0x8000) return rgba(expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31), 255);
    const unsigned a = (v >> 12) & 7;
    return rgba(expand4((v >> 8) & 15), expand4((v >> 4) & 15), expand4(v & 15), (a << 5) | (a << 2) | (a >> 1));
}
std::uint32_t ia8(unsigned v) { const unsigned i = v & 255; return rgba(i, i, i, v >> 8); }

std::uint32_t paletteColor(Bytes palette, GxPalette format, unsigned index, bool& valid) {
    if (!palette.data || !palette.contains(index * 2, 2)) { valid = false; return 0; }
    const unsigned v = be16(palette.data + index * 2);
    return format == GxPalette::IA8 ? ia8(v) : format == GxPalette::RGB565 ? rgb565(v) : rgb5a3(v);
}

// Hardware CMPR interpolates in eighths (5:3 toward the nearer endpoint), not thirds, as Dolphin measured.
unsigned blend(unsigned a, unsigned b) { return (a * 3 + b * 5) >> 3; }
std::uint32_t mix(std::uint32_t a, std::uint32_t b, unsigned alpha) {
    return rgba(blend(a >> 24, b >> 24), blend(a >> 16 & 255, b >> 16 & 255), blend(a >> 8 & 255, b >> 8 & 255), alpha);
}
std::uint32_t average(std::uint32_t a, std::uint32_t b, unsigned alpha) {
    return rgba(((a >> 24) + (b >> 24)) / 2, ((a >> 16 & 255) + (b >> 16 & 255)) / 2,
                ((a >> 8 & 255) + (b >> 8 & 255)) / 2, alpha);
}
} // namespace

std::size_t gxImageSize(GxFormat format, unsigned width, unsigned height) {
    Layout l;
    if (!layout(format, l)) return 0;
    return std::size_t((width + l.blockWidth - 1) / l.blockWidth) * ((height + l.blockHeight - 1) / l.blockHeight) * l.blockBytes;
}

void tileIntensity8(const std::uint8_t* gx, unsigned width, unsigned height, std::uint8_t* la8) {
    const auto morton = [](unsigned x, unsigned y) {
        return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    };
    for (unsigned y = 0; y < height; ++y) {
        const unsigned row = height - 1 - y;  // the PICA's, from the bottom
        for (unsigned x = 0; x < width; ++x) {
            const std::uint8_t i = gx[((y / 4) * (width / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8];
            const unsigned texel = ((row / 8) * (width / 8) + x / 8) * 64 + morton(x, row);
            la8[texel * 2] = la8[texel * 2 + 1] = i;
        }
    }
}

bool decodeGxImage(Bytes source, GxFormat format, unsigned width, unsigned height, Bytes palette,
                   GxPalette paletteFormat, Image& output, std::string& error) {
    output = {};
    error.clear();
    Layout l;
    if (!layout(format, l)) return fail(error, "Unknown GX texture format");
    if (!width || !height || width > 1024 || height > 1024) return fail(error, "Unsupported GX texture dimensions");
    if (!source.data || !source.contains(0, gxImageSize(format, width, height))) return fail(error, "Truncated GX texture");
    const bool indexed = format == GxFormat::C4 || format == GxFormat::C8 || format == GxFormat::C14X2;
    if (indexed && static_cast<unsigned>(paletteFormat) > 2) return fail(error, "Unknown GX palette format");
    output.width = width;
    output.height = height;
    output.pixels.assign(std::size_t(width) * height, 0);
    bool valid = true;
    const unsigned blocksX = (width + l.blockWidth - 1) / l.blockWidth;
    const unsigned blocksY = (height + l.blockHeight - 1) / l.blockHeight;
    for (unsigned by = 0; by < blocksY; ++by) for (unsigned bx = 0; bx < blocksX; ++bx) {
        const std::uint8_t* block = source.data + (std::size_t(by) * blocksX + bx) * l.blockBytes;
        const auto put = [&](unsigned x, unsigned y, std::uint32_t color) {
            const unsigned px = bx * l.blockWidth + x, py = by * l.blockHeight + y;
            if (px < width && py < height) output.pixels[std::size_t(py) * width + px] = color;
        };
        if (format == GxFormat::CMPR) {
            for (unsigned sub = 0; sub < 4; ++sub) {
                const std::uint8_t* s = block + sub * 8;
                const unsigned a = be16(s), b = be16(s + 2);
                std::uint32_t colors[4]{rgb565(a), rgb565(b), 0, 0};
                if (a > b) {
                    colors[2] = mix(colors[1], colors[0], 255);
                    colors[3] = mix(colors[0], colors[1], 255);
                } else {
                    // The fourth color is the average made transparent, not DXT1's black.
                    colors[2] = average(colors[0], colors[1], 255);
                    colors[3] = average(colors[0], colors[1], 0);
                }
                for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
                    put((sub % 2) * 4 + x, (sub / 2) * 4 + y, colors[s[4 + y] >> (6 - 2 * x) & 3]);
            }
            continue;
        }
        for (unsigned y = 0; y < l.blockHeight; ++y) for (unsigned x = 0; x < l.blockWidth; ++x) {
            const unsigned i = y * l.blockWidth + x;
            std::uint32_t c = 0;
            switch (format) {
            case GxFormat::I4: { const unsigned v = expand4(block[i / 2] >> (i % 2 ? 0 : 4) & 15); c = rgba(v, v, v, v); break; }
            case GxFormat::I8: c = rgba(block[i], block[i], block[i], block[i]); break;
            case GxFormat::IA4: { const unsigned v = expand4(block[i] & 15); c = rgba(v, v, v, expand4(block[i] >> 4)); break; }
            case GxFormat::IA8: c = ia8(be16(block + i * 2)); break;
            case GxFormat::RGB565: c = rgb565(be16(block + i * 2)); break;
            case GxFormat::RGB5A3: c = rgb5a3(be16(block + i * 2)); break;
            case GxFormat::RGBA8: c = rgba(block[i * 2 + 1], block[32 + i * 2], block[32 + i * 2 + 1], block[i * 2]); break;
            case GxFormat::C4: c = paletteColor(palette, paletteFormat, block[i / 2] >> (i % 2 ? 0 : 4) & 15, valid); break;
            case GxFormat::C8: c = paletteColor(palette, paletteFormat, block[i], valid); break;
            case GxFormat::C14X2: c = paletteColor(palette, paletteFormat, be16(block + i * 2) & 0x3fff, valid); break;
            default: break;
            }
            put(x, y, c);
        }
    }
    if (!valid) { output = {}; return fail(error, "GX palette index outside palette"); }
    output.grayscale = true;
    for (auto c : output.pixels) {
        if ((c >> 24) != (c >> 16 & 255) || (c >> 24) != (c >> 8 & 255)) { output.grayscale = false; break; }
    }
    return true;
}
} // namespace smg
