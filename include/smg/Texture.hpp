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
// Where mip level `level` starts: GX stores the levels one after another, each halved and padded to whole blocks.
std::size_t gxLevelOffset(GxFormat format, unsigned width, unsigned height, unsigned level);
// The levels a mipmapped texture gives the PICA, which samples none under 8x8: none past the max LOD (in sixteenths of a
// level, as TX_SETMODE1 holds it), and 1 for a size that isn't a power of two.
unsigned picaMipLevels(unsigned width, unsigned height, unsigned maxLod);
// An I8 image straight into the PICA's LA8 (both bytes the intensity, as GX gives it alpha too), rows bottom first in
// 8x8 Morton tiles, as decoding and the PICA tiler make it. Width and height powers of two, at least 8.
void tileIntensity8(const std::uint8_t* gx, unsigned width, unsigned height, std::uint8_t* la8);
// Decodes the first mip level. Palette formats need the palette's entries in `palette`.
bool decodeGxImage(Bytes source, GxFormat format, unsigned width, unsigned height, Bytes palette,
                   GxPalette paletteFormat, Image& output, std::string& error);
} // namespace smg
