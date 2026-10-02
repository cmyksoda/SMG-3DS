// Camera animations: a 0x20-byte header, eight components' (count, offset[, key type]) words, then the values as a
// byte count and floats, every word big-endian on the disc.
#include <smg/CameraAnim.hpp>
#include <cstring>

namespace smg {
namespace {
std::uint32_t swapped(std::uint32_t v) { return v >> 24 | (v >> 8 & 0xFF00) | (v << 8 & 0xFF0000) | v << 24; }
void turn(std::uint8_t* at, std::size_t words) {
    for (std::size_t i = 0; i < words; ++i) {
        std::uint32_t v;
        std::memcpy(&v, at + i * 4, 4);
        v = swapped(v);
        std::memcpy(at + i * 4, &v, 4);
    }
}
std::uint32_t big32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
} // namespace

const char* nativeCameraAnimation(std::uint8_t* file, std::size_t size) {
    if (size < 0x20 || std::memcmp(file, "ANDO", 4)) return "is not a camera animation";
    const bool keyed = !std::memcmp(file + 4, "CKAN", 4);
    if (!keyed && std::memcmp(file + 4, "CANM", 4)) return "is a camera animation of a kind the game doesn't read";
    const std::size_t info = keyed ? 8 * 12 : 8 * 8, entry = 0x20;
    const std::uint32_t valueOffset = big32(file + 0x1C);
    if (valueOffset < info || entry + std::size_t(valueOffset) + 4 > size) return "has its values outside the file";
    const std::uint32_t valueBytes = big32(file + entry + valueOffset);
    if (valueBytes % 4 || entry + std::size_t(valueOffset) + 4 + valueBytes > size) return "has more values than the file holds";
    for (unsigned c = 0; c < 8; ++c) {
        const std::uint8_t* component = file + entry + c * (keyed ? 12 : 8);
        const std::uint64_t count = big32(component), offset = big32(component + 4), stride = keyed ? (count == 1 ? 1 : big32(component + 8) ? 4 : 3) : 1;
        if ((offset + count * stride) * 4 > valueBytes) return "has a component reaching past its values";
    }
    turn(file + 8, 6);
    turn(file + entry, info / 4);
    turn(file + entry + valueOffset, 1 + valueBytes / 4);
    return nullptr;
}
} // namespace smg
