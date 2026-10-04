#pragma once
#include <smg/Archive.hpp>
#include <cstdint>

namespace smg {
// A THP movie frame's video component: a baseline JPEG, YCbCr 4:2:0 in 16x16 MCUs, whose scan is not byte-stuffed (an
// 0xFF in it is data). The planes come out 8-bit, row by row: luma width x height, each chroma plane half that each
// way. Returns why the frame can't be decoded, or null.
const char* decodeThpVideo(Bytes frame, unsigned width, unsigned height, std::uint8_t* y, std::uint8_t* u, std::uint8_t* v);
// A frame's size, from its frame header.
const char* thpVideoSize(Bytes frame, unsigned& width, unsigned& height);

// BT.601 full-range YCbCr, as THP's encoder wrote it, to RGB.
void thpPixel(std::uint8_t y, std::uint8_t u, std::uint8_t v, std::uint8_t rgb[3]);
// Decoded planes as an RGB565 texture of tw x th for the PICA (rows upside down, 8x8 tiles in Morton order), each texel
// thpPixel's narrowed.
void thpTiles565(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, std::uint16_t* texels,
                 unsigned tw, unsigned th);
} // namespace smg
