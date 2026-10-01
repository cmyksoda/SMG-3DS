#pragma once
#include <smg/GxCommands.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace smg {
// An EFB copy to a texture as the Wii's pixel engine is set up for it: the source rectangle in EFB pixels, where and
// how the texels go, and the copy's filters.
struct GxTextureCopy {
    unsigned x = 0, y = 0, width = 0, height = 0;
    std::uint32_t address = 0;
    unsigned texWidth = 0, texHeight = 0;  // half the source each way with the box filter
    std::uint8_t format = 0;               // the PE's 4-bit copy format
    bool intensity = false, halfScale = false, clear = false, clampTop = false, clampBottom = false;
    std::uint8_t filter[7]{};               // vertical taps over rows -3..3, in 64ths
    std::uint32_t stride = 0;               // bytes from one row of tiles to the next
};

// The copy that PE_COPY_CMD `command` starts, from the registers set before it; returns why the port can't make it, or
// null. A display copy (opcode 1) is not one.
const char* readTextureCopy(const GxCommandProcessor& gpu, std::uint32_t command, GxTextureCopy& out);

// Bytes the copy writes from its address: whole tiles, rows of them `stride` apart.
std::size_t textureCopySize(const GxTextureCopy& copy);

// Writes the copy's texels in its format's tiling. `pixel` gives an EFB pixel as RGBA; it is asked for pixels outside
// the source rectangle (by the filters and the tiles' edges) and should clamp to the EFB itself.
using EfbPixel = std::array<std::uint8_t, 4> (*)(const void* context, int x, int y);
void encodeTextureCopy(const GxTextureCopy& copy, EfbPixel pixel, const void* context, std::uint8_t* destination);
} // namespace smg
