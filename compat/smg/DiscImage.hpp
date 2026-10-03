#pragma once
// The game's disc as an image on the SD card (3ds/SMG-3DS/game.rvz, game.wbfs or game.iso) when there is one;
// otherwise the game reads the files extracted from it (3ds/SMG-3DS/game and sys).
#include <cstdint>
#include <vector>

namespace smg::disc {
bool fromImage();  // the first call opens it, and stops, naming why, if one is there but won't open
bool read(std::uint64_t offset, void* out, std::uint32_t size);  // the data partition's bytes, from any thread
const std::vector<std::uint8_t>& fileTable();
const std::uint8_t* bootBin();  // the partition's first 0x440 bytes
} // namespace smg::disc
