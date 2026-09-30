#pragma once
#include <smg/Texture.hpp>
#include <cstdint>
#include <vector>

namespace smg {
// ETC1 as the PICA200 samples it: 8x8 tiles of four 4x4 blocks (top-left, top-right,
// bottom-left, bottom-right), each block's 64-bit word stored little-endian. With alpha
// (ETC1A4), every block is preceded by sixteen 4-bit alpha values in its own 64-bit word.
// Width and height must be powers of two of at least 8.
std::vector<std::uint8_t> encodeEtc1(const Image& image, bool alpha);
Image decodeEtc1(const std::uint8_t* data, unsigned width, unsigned height, bool alpha);
} // namespace smg
