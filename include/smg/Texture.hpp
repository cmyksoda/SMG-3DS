#pragma once
#include <smg/Archive.hpp>
#include <cstdint>
#include <vector>

namespace smg {
enum class GxFormat : std::uint8_t {
    I4 = 0, I8 = 1, IA4 = 2, IA8 = 3, RGB565 = 4, RGB5A3 = 5, RGBA8 = 6,
    C4 = 8, C8 = 9, C14X2 = 10, CMPR = 14
};
enum class GxPalette : std::uint8_t { IA8 = 0, RGB565 = 1, RGB5A3 = 2 };

struct Image {
    unsigned width = 0, height = 0;
    // Row-major RGBA words (0xRRGGBBAA), top row first.
    std::vector<std::uint32_t> pixels;
    bool grayscale = false;         // every pixel has R == G == B
};

std::size_t gxImageSize(GxFormat format, unsigned width, unsigned height);
// Decodes the first mip level. Palette formats need the palette's entries in `palette`.
bool decodeGxImage(Bytes source, GxFormat format, unsigned width, unsigned height, Bytes palette,
                   GxPalette paletteFormat, Image& output, std::string& error);
} // namespace smg
