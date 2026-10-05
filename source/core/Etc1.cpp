#include <smg/Etc1.hpp>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace smg {
namespace {
constexpr int Modifiers[8][4]{
    {2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
    {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}};

struct Rgb { int r, g, b; };

int clamp255(int v) { return std::clamp(v, 0, 255); }
int expand4(int v) { return (v << 4) | v; }
int expand5(int v) { return (v << 3) | (v >> 2); }

// ETC1 numbers pixels down each column: x * 4 + y.
bool inFirst(unsigned x, unsigned y, bool flip) { return flip ? y < 2 : x < 2; }

struct Choice {
    long error = LONG_MAX;
    std::uint64_t word = 0;
};

// Best table and per-pixel modifiers for one half-block around a fixed base color. A modifier moves all three channels
// equally, so each pixel takes the one nearest its mean offset (by the midpoints between them). Only the three tables
// whose large modifier is nearest the half's widest offset are tried: all eight cost a scene's first frames seconds.
// Each half's eight pixels (x * 4 + y), by flip and then first.
constexpr std::uint8_t HalfPixels[2][2][8] = {{{8, 9, 10, 11, 12, 13, 14, 15}, {0, 1, 2, 3, 4, 5, 6, 7}},
                                              {{2, 3, 6, 7, 10, 11, 14, 15}, {0, 1, 4, 5, 8, 9, 12, 13}}};

long fitHalf(const Rgb block[16], bool flip, bool first, Rgb base, unsigned& table, std::uint32_t& indices) {
    int offsets[8], widest = 0;
    const std::uint8_t* positions = HalfPixels[flip][first];
    const unsigned count = 8;
    for (unsigned i = 0; i < count; ++i) {
        const Rgb& p = block[positions[i]];
        offsets[i] = (p.r - base.r + p.g - base.g + p.b - base.b) / 3;
        widest = std::max(widest, std::abs(offsets[i]));
    }
    unsigned center = 0;
    for (unsigned t = 1; t < 8; ++t)
        if (std::abs(Modifiers[t][1] - widest) < std::abs(Modifiers[center][1] - widest)) center = t;
    long best = LONG_MAX;
    for (unsigned t = center ? center - 1 : 0; t <= std::min(center + 1, 7u); ++t) {
        const int middle = (Modifiers[t][0] + Modifiers[t][1]) / 2;
        long total = 0;
        std::uint32_t chosen = 0;
        for (unsigned i = 0; i < count; ++i) {
            const int o = offsets[i];
            const unsigned pick = o >= middle ? 1 : o >= 0 ? 0 : o >= -middle ? 2 : 3;
            const Rgb& p = block[positions[i]];
            const int d = Modifiers[t][pick];
            const long dr = clamp255(base.r + d) - p.r, dg = clamp255(base.g + d) - p.g, db = clamp255(base.b + d) - p.b;
            total += dr * dr + dg * dg + db * db;
            chosen |= pick << (2 * positions[i]);
            if (total >= best) break;
        }
        if (total < best) { best = total; table = t; indices = chosen; }
    }
    return best;
}

// A block's four 2x2 quadrants (x / 2, y / 2): their channel sums and sums of squares, from which either split's
// averages and error follow without visiting a pixel again.
struct Quadrants {
    long sum[2][2][3] = {}, squares[2][2][3] = {};
    explicit Quadrants(const Rgb block[16]) {
        for (unsigned x = 0; x < 4; ++x) for (unsigned y = 0; y < 4; ++y) {
            const Rgb& p = block[x * 4 + y];
            const int c[3] = {p.r, p.g, p.b};
            for (int k = 0; k < 3; ++k) sum[x / 2][y / 2][k] += c[k], squares[x / 2][y / 2][k] += long(c[k]) * c[k];
        }
    }
    // The two quadrants a half holds: by x for the first split, by y for the flipped one.
    long half(const long (&of)[2][2][3], bool flip, bool first, int k) const {
        const unsigned i = first ? 0 : 1;
        return flip ? of[0][i][k] + of[1][i][k] : of[i][0][k] + of[i][1][k];
    }
    Rgb average(bool flip, bool first) const {
        return {int(half(sum, flip, first, 0) / 8), int(half(sum, flip, first, 1) / 8), int(half(sum, flip, first, 2) / 8)};
    }
    // How far the pixels sit from their own half's average, split one way or the other: the split to fit.
    long splitError(bool flip) const {
        long error = 0;
        for (bool first : {true, false}) {
            const Rgb m = average(flip, first);
            const int mean[3] = {m.r, m.g, m.b};
            for (int k = 0; k < 3; ++k) error += half(squares, flip, first, k) - 2 * mean[k] * half(sum, flip, first, k) + 8L * mean[k] * mean[k];
        }
        return error;
    }
};

std::uint64_t pack(bool differential, bool flip, const int c1[3], const int c2[3], unsigned t1, unsigned t2,
                   std::uint32_t i1, std::uint32_t i2) {
    std::uint64_t w = 0;
    for (int c = 0; c < 3; ++c) {
        const int shift = 59 - c * 8;
        if (differential) {
            w |= std::uint64_t(c1[c] & 31) << shift;
            w |= std::uint64_t((c2[c] - c1[c]) & 7) << (shift - 3);
        } else {
            w |= std::uint64_t(c1[c] & 15) << (shift + 1);
            w |= std::uint64_t(c2[c] & 15) << (shift - 3);
        }
    }
    w |= std::uint64_t(t1) << 37 | std::uint64_t(t2) << 34 | std::uint64_t(differential) << 33 | std::uint64_t(flip) << 32;
    // Merge the two halves' 2-bit indices into the MSB and LSB planes.
    const std::uint32_t both = i1 | i2;
    std::uint32_t msb = 0, lsb = 0;
    for (unsigned p = 0; p < 16; ++p) {
        const unsigned index = both >> (2 * p) & 3;
        msb |= (index >> 1) << p;
        lsb |= (index & 1) << p;
    }
    return w | std::uint64_t(msb) << 16 | lsb;
}

// One split, chosen by its halves' fit, and one mode: differential when the halves are close enough, else individual.
// The exhaustive search over both took four times as long, which first frames of large scenes felt.
std::uint64_t encodeBlock(const Rgb block[16]) {
    Choice best;
    const Quadrants quadrants(block);
    const bool chosen = quadrants.splitError(true) < quadrants.splitError(false);
    for (bool flip : {chosen}) {
        const Rgb a = quadrants.average(flip, true), b = quadrants.average(flip, false);
        // Differential mode keeps five bits per channel when the halves are close enough.
        const int d1[3]{a.r * 31 / 255, a.g * 31 / 255, a.b * 31 / 255};
        const int d2[3]{b.r * 31 / 255, b.g * 31 / 255, b.b * 31 / 255};
        bool fits = true;
        for (int c = 0; c < 3; ++c) fits &= d2[c] - d1[c] >= -4 && d2[c] - d1[c] <= 3;
        const int i1[3]{(a.r * 15 + 127) / 255, (a.g * 15 + 127) / 255, (a.b * 15 + 127) / 255};
        const int i2[3]{(b.r * 15 + 127) / 255, (b.g * 15 + 127) / 255, (b.b * 15 + 127) / 255};
        for (bool differential : {true, false}) {
            if (differential && !fits) continue;
            const int* c1 = differential ? d1 : i1;
            const int* c2 = differential ? d2 : i2;
            const auto expand = differential ? expand5 : expand4;
            unsigned t1 = 0, t2 = 0;
            std::uint32_t x1 = 0, x2 = 0;
            const long error = fitHalf(block, flip, true, {expand(c1[0]), expand(c1[1]), expand(c1[2])}, t1, x1) +
                               fitHalf(block, flip, false, {expand(c2[0]), expand(c2[1]), expand(c2[2])}, t2, x2);
            if (error < best.error) best = {error, pack(differential, flip, c1, c2, t1, t2, x1, x2)};
            break;
        }
    }
    return best.word;
}

void decodeBlock(std::uint64_t w, std::uint32_t out[16], std::uint64_t alphaWord, bool alpha) {
    const bool differential = w >> 33 & 1, flip = w >> 32 & 1;
    Rgb base[2];
    int channels[2][3];
    for (int c = 0; c < 3; ++c) {
        const int shift = 59 - c * 8;
        if (differential) {
            const int first = w >> shift & 31;
            int delta = w >> (shift - 3) & 7;
            if (delta > 3) delta -= 8;
            channels[0][c] = expand5(first);
            channels[1][c] = expand5((first + delta) & 31);
        } else {
            channels[0][c] = expand4(w >> (shift + 1) & 15);
            channels[1][c] = expand4(w >> (shift - 3) & 15);
        }
    }
    for (int h = 0; h < 2; ++h) base[h] = {channels[h][0], channels[h][1], channels[h][2]};
    const unsigned tables[2]{unsigned(w >> 37 & 7), unsigned(w >> 34 & 7)};
    for (unsigned x = 0; x < 4; ++x) for (unsigned y = 0; y < 4; ++y) {
        const unsigned p = x * 4 + y;
        const int half = inFirst(x, y, flip) ? 0 : 1;
        const unsigned index = (w >> (16 + p) & 1) << 1 | (w >> p & 1);
        const int d = Modifiers[tables[half]][index];
        const unsigned a = alpha ? unsigned(alphaWord >> (4 * p) & 15) * 17 : 255;
        out[y * 4 + x] = std::uint32_t(clamp255(base[half].r + d)) << 24 | std::uint32_t(clamp255(base[half].g + d)) << 16 |
                         std::uint32_t(clamp255(base[half].b + d)) << 8 | a;
    }
}

void store64(std::uint8_t* at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) at[i] = std::uint8_t(v >> (8 * i));
}
std::uint64_t load64(const std::uint8_t* at) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(at[i]) << (8 * i);
    return v;
}

// Blocks within an 8x8 tile go top-left, top-right, bottom-left, bottom-right.
template<typename Visit>
void forEachBlock(unsigned width, unsigned height, Visit visit) {
    std::size_t index = 0;
    for (unsigned ty = 0; ty < height; ty += 8) for (unsigned tx = 0; tx < width; tx += 8)
        for (unsigned b = 0; b < 4; ++b) visit(tx + (b & 1) * 4, ty + (b >> 1) * 4, index++);
}
} // namespace

std::vector<std::uint8_t> encodeEtc1(const Image& image, bool alpha) {
    const unsigned stride = alpha ? 16 : 8;
    std::vector<std::uint8_t> out(std::size_t(image.width / 4) * (image.height / 4) * stride);
    forEachBlock(image.width, image.height, [&](unsigned bx, unsigned by, std::size_t index) {
        Rgb block[16];
        std::uint64_t alphaWord = 0;
        for (unsigned x = 0; x < 4; ++x) for (unsigned y = 0; y < 4; ++y) {
            const std::uint32_t c = image.pixels[std::size_t(by + y) * image.width + bx + x];
            block[x * 4 + y] = {int(c >> 24), int(c >> 16 & 255), int(c >> 8 & 255)};
            alphaWord |= std::uint64_t(((c & 255) + 8) * 15 / 255) << (4 * (x * 4 + y));
        }
        std::uint8_t* at = out.data() + index * stride;
        if (alpha) {
            store64(at, alphaWord);
            at += 8;
        }
        store64(at, encodeBlock(block));
    });
    return out;
}

Image decodeEtc1(const std::uint8_t* data, unsigned width, unsigned height, bool alpha) {
    Image image;
    image.width = width;
    image.height = height;
    image.pixels.assign(std::size_t(width) * height, 0);
    const unsigned stride = alpha ? 16 : 8;
    forEachBlock(width, height, [&](unsigned bx, unsigned by, std::size_t index) {
        const std::uint8_t* at = data + index * stride;
        const std::uint64_t alphaWord = alpha ? load64(at) : 0;
        std::uint32_t block[16];
        decodeBlock(load64(at + (alpha ? 8 : 0)), block, alphaWord, alpha);
        for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x)
            image.pixels[std::size_t(by + y) * width + bx + x] = block[y * 4 + x];
    });
    return image;
}
} // namespace smg
