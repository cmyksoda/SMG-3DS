#include <smg/GxCopy.hpp>
#include <private/pe_reg.h>
#include <algorithm>
#include <vector>

namespace smg {
namespace {
struct Tiling {
    unsigned width, height, bytes;
};

Tiling tiling(std::uint8_t format) {
    switch (format) {
    case 0: return {8, 8, 32};                        // 4 bits a texel
    case 3: case 4: case 5: case 11: case 12: return {4, 4, 32};  // 16
    case 6: return {4, 4, 64};                        // RGBA8: an AR block, then a GB block
    default: return {8, 4, 32};                       // 8
    }
}

// The pixel engine's RGB to Y, BT.601 studio range: white comes out 235.
std::uint8_t luma(const std::array<std::uint8_t, 4>& c) { return static_cast<std::uint8_t>(((66 * c[0] + 129 * c[1] + 25 * c[2] + 128) >> 8) + 16); }
} // namespace

const char* readTextureCopy(const GxCommandProcessor& gpu, std::uint32_t command, GxTextureCopy& out) {
    if (PE_COPY_CMD_GET_OPCODE(command)) return "a display copy taken as a texture copy";
    const std::uint32_t source = gpu.bp[0x49], size = gpu.bp[0x4A], filter0 = gpu.bp[0x53], filter1 = gpu.bp[0x54];
    out = {};
    out.x = PE_COPY_SRC_ADDR_GET_X(source), out.y = PE_COPY_SRC_ADDR_GET_Y(source);
    out.width = PE_COPY_SRC_SIZE_GET_X(size) + 1, out.height = PE_COPY_SRC_SIZE_GET_Y(size) + 1;
    out.address = PE_COPY_DST_BASE_GET_BASE(gpu.bp[0x4B]) << 5;
    out.stride = PE_COPY_DST_STRIDE_GET_STRIDE(gpu.bp[0x4D]) << 5;
    out.format = static_cast<std::uint8_t>(PE_COPY_CMD_GET_TEX_FORMATH(command) << 3 | PE_COPY_CMD_GET_TEX_FORMAT(command));
    out.intensity = PE_COPY_CMD_GET_CCV(command) == 3;
    out.halfScale = PE_COPY_CMD_GET_MIP_MAP_FILTER(command);
    out.clear = PE_COPY_CMD_GET_CLEAR(command);
    out.clampTop = PE_COPY_CMD_GET_CLAMP_TOP(command), out.clampBottom = PE_COPY_CMD_GET_CLAMP_BOTTOM(command);
    out.depth = PE_CONTROL_GET_PIXTYPE(gpu.bp[0x43]) == 3;
    const std::uint32_t taps[7] = {PE_COPY_VFILTER0_GET_COEFF0(filter0), PE_COPY_VFILTER0_GET_COEFF1(filter0), PE_COPY_VFILTER0_GET_COEFF2(filter0),
                                   PE_COPY_VFILTER0_GET_COEFF3(filter0), PE_COPY_VFILTER1_GET_COEFF4(filter1), PE_COPY_VFILTER1_GET_COEFF5(filter1),
                                   PE_COPY_VFILTER1_GET_COEFF6(filter1)};
    for (int i = 0; i < 7; ++i) out.filter[i] = static_cast<std::uint8_t>(taps[i]);
    out.texWidth = out.halfScale ? out.width / 2 : out.width, out.texHeight = out.halfScale ? out.height / 2 : out.height;
    if (!out.texWidth || !out.texHeight) return "an empty texture copy";
    if (out.format > 12) return "an EFB copy to an unknown format";
    if (out.intensity && out.format == 6 && !out.depth) return "an EFB copy to YUVA8";
    // Z4, Z8 (twice), Z16, Z24X8, Z8M, Z8L and Z16L; the rest have no depth meaning.
    if (out.depth && (out.format == 2 || out.format == 4 || out.format == 5 || out.format == 7 || out.format == 11))
        return "an EFB depth copy to a color format";
    const Tiling t = tiling(out.format);
    if (out.stride < (out.texWidth + t.width - 1) / t.width * t.bytes) return "a texture copy whose rows of tiles overlap";
    return nullptr;
}

std::size_t textureCopySize(const GxTextureCopy& copy) {
    const Tiling t = tiling(copy.format);
    const std::size_t across = (copy.texWidth + t.width - 1) / t.width, down = (copy.texHeight + t.height - 1) / t.height;
    return (down - 1) * copy.stride + across * t.bytes;
}

void encodeTextureCopy(const GxTextureCopy& copy, EfbPixel pixel, const void* context, std::uint8_t* destination) {
    const Tiling t = tiling(copy.format);
    const unsigned across = (copy.texWidth + t.width - 1) / t.width, down = (copy.texHeight + t.height - 1) / t.height;
    const unsigned columns = across * t.width, rows = down * t.height;
    // Each destination row the filters may reach, box-filtered once: rows -3 .. rows+2, clamped as the copy says.
    std::vector<std::array<std::uint8_t, 4>> boxed(std::size_t(columns) * (rows + 6));
    for (int row = -3; row < int(rows) + 3; ++row) {
        int from = row;
        if (copy.clampTop && from < 0) from = 0;
        if (copy.clampBottom && from >= int(copy.texHeight)) from = int(copy.texHeight) - 1;
        for (unsigned column = 0; column < columns; ++column) {
            std::array<std::uint8_t, 4>& out = boxed[std::size_t(row + 3) * columns + column];
            if (!copy.halfScale) {
                out = pixel(context, int(copy.x + column), int(copy.y) + from);
                continue;
            }
            if (copy.depth) {  // a depth's bytes average as one number
                std::uint32_t z = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const auto p = pixel(context, int(copy.x + 2 * column) + dx, int(copy.y) + 2 * from + dy);
                        z += std::uint32_t(p[0]) << 16 | p[1] << 8 | p[2];
                    }
                z = (z + 2) / 4;
                out = {static_cast<std::uint8_t>(z >> 16), static_cast<std::uint8_t>(z >> 8), static_cast<std::uint8_t>(z), 255};
                continue;
            }
            unsigned sum[4] = {};
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const auto p = pixel(context, int(copy.x + 2 * column) + dx, int(copy.y) + 2 * from + dy);
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            for (int c = 0; c < 4; ++c) out[c] = static_cast<std::uint8_t>((sum[c] + 2) / 4);
        }
    }
    const auto texel = [&](unsigned column, unsigned row) {
        if (copy.depth) return boxed[std::size_t(row + 3) * columns + column];  // the copy filter blends color only
        unsigned sum[4] = {};
        for (int tap = 0; tap < 7; ++tap) {
            if (!copy.filter[tap]) continue;
            const auto& p = boxed[std::size_t(int(row) + tap) * columns + column];  // row + (tap - 3), offset by 3
            for (int c = 0; c < 4; ++c) sum[c] += copy.filter[tap] * p[c];
        }
        std::array<std::uint8_t, 4> c;
        for (int i = 0; i < 4; ++i) c[i] = static_cast<std::uint8_t>(std::min(255u, sum[i] >> 6));
        return c;
    };

    for (unsigned tileY = 0; tileY < down; ++tileY) {
        for (unsigned tileX = 0; tileX < across; ++tileX) {
            std::uint8_t* tile = destination + std::size_t(tileY) * copy.stride + std::size_t(tileX) * t.bytes;
            for (unsigned y = 0; y < t.height; ++y) {
                for (unsigned x = 0; x < t.width; ++x) {
                    const auto c = texel(tileX * t.width + x, tileY * t.height + y);
                    const std::uint8_t i = copy.intensity && !copy.depth ? luma(c) : c[0];
                    const unsigned n = y * t.width + x;
                    if (copy.depth) {
                        switch (copy.format) {
                        case 0: tile[n / 2] = static_cast<std::uint8_t>(n & 1 ? (tile[n / 2] & 0xF0) | c[0] >> 4 : (tile[n / 2] & 0x0F) | (c[0] & 0xF0)); break;
                        case 1: case 8: tile[n] = c[0]; break;              // Z8: the top byte
                        case 9: tile[n] = c[1]; break;                      // Z8M
                        case 10: tile[n] = c[2]; break;                     // Z8L
                        case 3: tile[2 * n] = c[0], tile[2 * n + 1] = c[1]; break;   // Z16, big-endian as the Wii reads it
                        case 12: tile[2 * n] = c[1], tile[2 * n + 1] = c[2]; break;  // Z16L
                        case 6:                                             // Z24X8: an AR block (X, then z's top), a GB block
                            tile[2 * n] = 255, tile[2 * n + 1] = c[0];
                            tile[32 + 2 * n] = c[1], tile[32 + 2 * n + 1] = c[2];
                            break;
                        }
                        continue;
                    }
                    switch (copy.format) {
                    case 0: tile[n / 2] = static_cast<std::uint8_t>(n & 1 ? (tile[n / 2] & 0xF0) | i >> 4 : (tile[n / 2] & 0x0F) | (i & 0xF0)); break;
                    case 1: case 8: tile[n] = i; break;
                    case 2: tile[n] = static_cast<std::uint8_t>((c[3] & 0xF0) | i >> 4); break;
                    case 3: tile[2 * n] = c[3], tile[2 * n + 1] = i; break;
                    case 4: {
                        const unsigned v = (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3;
                        tile[2 * n] = static_cast<std::uint8_t>(v >> 8), tile[2 * n + 1] = static_cast<std::uint8_t>(v);
                        break;
                    }
                    case 5: {
                        // Opaque texels keep five bits a channel; the rest give one to a three-bit alpha.
                        const unsigned v = c[3] >> 5 == 7 ? 0x8000u | (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3
                                                          : (c[3] >> 5) << 12 | (c[0] >> 4) << 8 | (c[1] >> 4) << 4 | c[2] >> 4;
                        tile[2 * n] = static_cast<std::uint8_t>(v >> 8), tile[2 * n + 1] = static_cast<std::uint8_t>(v);
                        break;
                    }
                    case 6:
                        tile[2 * n] = c[3], tile[2 * n + 1] = c[0];
                        tile[32 + 2 * n] = c[1], tile[32 + 2 * n + 1] = c[2];
                        break;
                    case 7: tile[n] = c[3]; break;
                    case 9: tile[n] = c[1]; break;
                    case 10: tile[n] = c[2]; break;
                    // Two channels a texel, read back as IA8 (alpha byte first): green then red, blue then green.
                    case 11: tile[2 * n] = c[1], tile[2 * n + 1] = c[0]; break;
                    case 12: tile[2 * n] = c[2], tile[2 * n + 1] = c[1]; break;
                    }
                }
            }
        }
    }
}
} // namespace smg
