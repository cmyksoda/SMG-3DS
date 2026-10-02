// THP movie frames: baseline JPEG as Nintendo's THP encoder wrote it (4:2:0, two Huffman table pairs, no byte stuffing),
// decoded into planes for the port's movie drawing. The Wii SDK's decoder is PowerPC assembly over the locked cache.
#include <smg/Thp.hpp>
#include <algorithm>
#include <array>
#include <cstring>

namespace smg {
namespace {
constexpr std::uint8_t ZigZag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                     41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                     30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct Huffman {
    bool present = false;
    // Canonical codes by length: the largest code of each length and where its values start.
    std::int32_t maxCode[18]{}, valueOffset[18]{};
    std::uint8_t values[256]{};
    // The first 9 bits looked up at once: value and length, or length 0 for a longer code.
    std::uint8_t fastValue[512]{}, fastLength[512]{};
};

bool buildHuffman(const std::uint8_t* counts, const std::uint8_t* values, unsigned total, Huffman& h) {
    if (total > 256) return false;
    std::memcpy(h.values, values, total);
    std::memset(h.fastLength, 0, sizeof h.fastLength);
    std::int32_t code = 0;
    unsigned k = 0;
    for (int length = 1; length <= 16; ++length) {
        h.valueOffset[length] = static_cast<std::int32_t>(k) - code;
        for (unsigned i = 0; i < counts[length - 1]; ++i, ++k, ++code) {
            if (length <= 9) {
                const unsigned first = static_cast<unsigned>(code) << (9 - length);
                for (unsigned fill = 0; fill < (1u << (9 - length)); ++fill)
                    h.fastValue[first + fill] = values[k], h.fastLength[first + fill] = static_cast<std::uint8_t>(length);
            }
        }
        h.maxCode[length] = counts[length - 1] ? code - 1 : -1;
        if (code > (1 << length)) return false;
        code <<= 1;
    }
    h.maxCode[17] = 0x7FFFFFFF;
    h.present = true;
    return true;
}

// The scan read MSB first, raw: THP writes no stuffed zero after an 0xFF. Past the end it reads zeros.
struct Bits {
    const std::uint8_t* data;
    std::size_t size, at = 0;
    std::uint32_t buffer = 0;
    int count = 0;
    void fill() {
        while (count <= 24) {
            buffer |= std::uint32_t(at < size ? data[at] : 0) << (24 - count);
            ++at, count += 8;
        }
    }
    unsigned peek(int n) { return fill(), buffer >> (32 - n); }
    void skip(int n) { buffer <<= n, count -= n; }
    unsigned take(int n) {
        if (!n) return 0;
        const unsigned value = peek(n);
        skip(n);
        return value;
    }
};

int decodeSymbol(Bits& bits, const Huffman& h) {
    const unsigned fast = bits.peek(9);
    if (const int length = h.fastLength[fast]) return bits.skip(length), h.fastValue[fast];
    for (int length = 10; length <= 16; ++length) {
        const std::int32_t code = static_cast<std::int32_t>(bits.peek(length));
        if (code <= h.maxCode[length]) return bits.skip(length), h.values[h.valueOffset[length] + code];
    }
    return -1;
}

int extend(unsigned value, int size) { return size && value < (1u << (size - 1)) ? int(value) - (1 << size) + 1 : int(value); }

// libjpeg's accurate integer IDCT (jidctint.c, 13-bit constants), with the level shift and clamping.
void inverseDct(const int* in, std::uint8_t* out, std::size_t stride) {
    constexpr int ConstBits = 13, Pass1Bits = 2;
    constexpr int F0_298 = 2446, F0_390 = 3196, F0_541 = 4433, F0_765 = 6270, F0_899 = 7373, F1_175 = 9633, F1_501 = 12299,
                  F1_847 = 15137, F1_961 = 16069, F2_053 = 16819, F2_562 = 20995, F3_072 = 25172;
    int work[64];
    for (int c = 0; c < 8; ++c) {
        const int* col = in + c;
        if (!col[8] && !col[16] && !col[24] && !col[32] && !col[40] && !col[48] && !col[56]) {
            const int dc = col[0] * (1 << Pass1Bits);
            for (int r = 0; r < 8; ++r) work[r * 8 + c] = dc;
            continue;
        }
        int z2 = col[16], z3 = col[48];
        int z1 = (z2 + z3) * F0_541;
        int tmp2 = z1 + z3 * -F1_847, tmp3 = z1 + z2 * F0_765;
        z2 = col[0], z3 = col[32];
        int tmp0 = (z2 + z3) * (1 << ConstBits), tmp1 = (z2 - z3) * (1 << ConstBits);
        const int tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = col[56], tmp1 = col[40], tmp2 = col[24], tmp3 = col[8];
        z1 = tmp0 + tmp3, z2 = tmp1 + tmp2, z3 = tmp0 + tmp2;
        int z4 = tmp1 + tmp3;
        const int z5 = (z3 + z4) * F1_175;
        tmp0 *= F0_298, tmp1 *= F2_053, tmp2 *= F3_072, tmp3 *= F1_501;
        z1 *= -F0_899, z2 *= -F2_562, z3 = z3 * -F1_961 + z5, z4 = z4 * -F0_390 + z5;
        tmp0 += z1 + z3, tmp1 += z2 + z4, tmp2 += z2 + z3, tmp3 += z1 + z4;
        const auto descale = [](int x) { return (x + (1 << (ConstBits - Pass1Bits - 1))) >> (ConstBits - Pass1Bits); };
        work[0 * 8 + c] = descale(tmp10 + tmp3), work[7 * 8 + c] = descale(tmp10 - tmp3);
        work[1 * 8 + c] = descale(tmp11 + tmp2), work[6 * 8 + c] = descale(tmp11 - tmp2);
        work[2 * 8 + c] = descale(tmp12 + tmp1), work[5 * 8 + c] = descale(tmp12 - tmp1);
        work[3 * 8 + c] = descale(tmp13 + tmp0), work[4 * 8 + c] = descale(tmp13 - tmp0);
    }
    for (int r = 0; r < 8; ++r) {
        const int* row = work + r * 8;
        std::uint8_t* o = out + r * stride;
        const auto pixel = [](int x) {
            const int v = ((x + (1 << (ConstBits + Pass1Bits + 2))) >> (ConstBits + Pass1Bits + 3)) + 128;
            return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
        };
        int z2 = row[2], z3 = row[6];
        int z1 = (z2 + z3) * F0_541;
        int tmp2 = z1 + z3 * -F1_847, tmp3 = z1 + z2 * F0_765;
        int tmp0 = (row[0] + row[4]) * (1 << ConstBits), tmp1 = (row[0] - row[4]) * (1 << ConstBits);
        const int tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3, tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        tmp0 = row[7], tmp1 = row[5], tmp2 = row[3], tmp3 = row[1];
        z1 = tmp0 + tmp3, z2 = tmp1 + tmp2, z3 = tmp0 + tmp2;
        int z4 = tmp1 + tmp3;
        const int z5 = (z3 + z4) * F1_175;
        tmp0 *= F0_298, tmp1 *= F2_053, tmp2 *= F3_072, tmp3 *= F1_501;
        z1 *= -F0_899, z2 *= -F2_562, z3 = z3 * -F1_961 + z5, z4 = z4 * -F0_390 + z5;
        tmp0 += z1 + z3, tmp1 += z2 + z4, tmp2 += z2 + z3, tmp3 += z1 + z4;
        o[0] = pixel(tmp10 + tmp3), o[7] = pixel(tmp10 - tmp3), o[1] = pixel(tmp11 + tmp2), o[6] = pixel(tmp11 - tmp2);
        o[2] = pixel(tmp12 + tmp1), o[5] = pixel(tmp12 - tmp1), o[3] = pixel(tmp13 + tmp0), o[4] = pixel(tmp13 - tmp0);
    }
}

std::uint16_t big16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
} // namespace

const char* decodeThpVideo(Bytes frame, unsigned width, unsigned height, std::uint8_t* y, std::uint8_t* u, std::uint8_t* v) {
    const std::uint8_t* d = frame.data;
    if (!frame.contains(0, 2) || d[0] != 0xFF || d[1] != 0xD8) return "a THP frame without a JPEG start";
    std::array<std::array<std::uint16_t, 64>, 4> quant{};
    std::array<Huffman, 4> tables{};  // DC 0, DC 1, AC 0, AC 1
    struct Component {
        unsigned quant = 0, dc = 0, ac = 0;
        int predictor = 0;
    } comps[3];
    bool frameSeen = false;
    std::size_t at = 2;
    for (;;) {
        if (!frame.contains(at, 4) || d[at] != 0xFF) return "a THP frame whose markers run out before its scan";
        const std::uint8_t marker = d[at + 1];
        const std::size_t length = big16(d + at + 2);
        if (length < 2 || !frame.contains(at + 2, length)) return "a THP frame with a segment past its end";
        const std::uint8_t* s = d + at + 4;
        const std::size_t n = length - 2;
        if (marker == 0xDB) {
            for (std::size_t i = 0; i < n; i += 65) {
                if (i + 65 > n || (s[i] >> 4) != 0 || (s[i] & 15) > 3) return "a THP frame with a quantization table it can't take";
                for (int k = 0; k < 64; ++k) quant[s[i] & 15][ZigZag[k]] = s[i + 1 + k];
            }
        } else if (marker == 0xC4) {
            for (std::size_t i = 0; i < n;) {
                if (i + 17 > n || (s[i] >> 4) > 1 || (s[i] & 15) > 1) return "a THP frame with a Huffman table it can't take";
                unsigned total = 0;
                for (int k = 0; k < 16; ++k) total += s[i + 1 + k];
                if (i + 17 + total > n || !buildHuffman(s + i + 1, s + i + 17, total, tables[(s[i] >> 4) * 2 + (s[i] & 15)]))
                    return "a THP frame with a malformed Huffman table";
                i += 17 + total;
            }
        } else if (marker == 0xC0) {
            if (n != 15 || s[0] != 8 || big16(s + 1) != height || big16(s + 3) != width || s[5] != 3)
                return "a THP frame whose size or components differ from its movie's";
            if (s[7] != 0x22 || s[10] != 0x11 || s[13] != 0x11) return "a THP frame not sampled 4:2:0";
            for (int c = 0; c < 3; ++c) comps[c].quant = s[8 + c * 3] & 3;
            frameSeen = true;
        } else if (marker == 0xDD) {
            if (n != 2 || big16(s)) return "a THP frame with restart intervals";
        } else if (marker == 0xDA) {
            if (!frameSeen || n != 10 || s[0] != 3 || s[7] != 0 || s[8] != 63 || s[9] != 0) return "a THP scan that is not one baseline scan";
            for (int c = 0; c < 3; ++c) {
                comps[c].dc = s[2 + c * 2] >> 4, comps[c].ac = s[2 + c * 2] & 15;
                if (comps[c].dc > 1 || comps[c].ac > 1 || !tables[comps[c].dc].present || !tables[2 + comps[c].ac].present)
                    return "a THP scan naming a Huffman table it lacks";
            }
            at += 2 + length;
            break;
        } else if (marker != 0xE0 && marker != 0xFE && (marker < 0xE1 || marker > 0xEF)) {
            return "a THP frame with a JPEG marker it can't take";
        }
        at += 2 + length;
    }
    if (width % 16 || height % 16 || !width || !height) return "a THP movie not a whole number of 16x16 blocks";

    Bits bits{d + at, frame.size - at};
    int coefficients[64];
    std::uint8_t* planes[3] = {y, u, v};
    const unsigned strides[3] = {width, width / 2, width / 2};
    for (unsigned my = 0; my < height / 16; ++my) {
        for (unsigned mx = 0; mx < width / 16; ++mx) {
            for (int block = 0; block < 6; ++block) {
                const int c = block < 4 ? 0 : block - 3;
                Component& comp = comps[c];
                std::memset(coefficients, 0, sizeof coefficients);
                const int dcSize = decodeSymbol(bits, tables[comp.dc]);
                if (dcSize < 0 || dcSize > 11) return "a THP frame with a bad DC code";
                comp.predictor += extend(bits.take(dcSize), dcSize);
                coefficients[0] = comp.predictor * quant[comp.quant][0];
                for (int k = 1; k < 64;) {
                    const int symbol = decodeSymbol(bits, tables[2 + comp.ac]);
                    if (symbol < 0) return "a THP frame with a bad AC code";
                    const int run = symbol >> 4, size = symbol & 15;
                    if (!size) {
                        if (run != 15) break;  // end of block
                        k += 16;
                        continue;
                    }
                    k += run;
                    if (k > 63) return "a THP frame whose block runs past 64 coefficients";
                    coefficients[ZigZag[k]] = extend(bits.take(size), size) * quant[comp.quant][ZigZag[k]];
                    ++k;
                }
                const unsigned bx = c ? mx * 8 : mx * 16 + (block & 1) * 8, by = c ? my * 8 : my * 16 + (block >> 1) * 8;
                inverseDct(coefficients, planes[c] + std::size_t(by) * strides[c] + bx, strides[c]);
            }
        }
    }
    return nullptr;
}

const char* thpVideoSize(Bytes frame, unsigned& width, unsigned& height) {
    const std::uint8_t* d = frame.data;
    if (!frame.contains(0, 2) || d[0] != 0xFF || d[1] != 0xD8) return "a THP frame without a JPEG start";
    for (std::size_t at = 2; frame.contains(at, 4) && d[at] == 0xFF && d[at + 1] != 0xDA; at += 2 + big16(d + at + 2)) {
        if (d[at + 1] == 0xC0 && frame.contains(at + 4, 5)) {
            height = big16(d + at + 5), width = big16(d + at + 7);
            return nullptr;
        }
    }
    return "a THP frame without a baseline frame header";
}

void thpPixel(std::uint8_t y, std::uint8_t u, std::uint8_t v, std::uint8_t rgb[3]) {
    const int cb = int(u) - 128, cr = int(v) - 128, luma = int(y) << 16;
    const auto clamp = [](int x) { return static_cast<std::uint8_t>(std::clamp((x + 0x8000) >> 16, 0, 255)); };
    rgb[0] = clamp(luma + 91881 * cr);
    rgb[1] = clamp(luma - 22554 * cb - 46802 * cr);
    rgb[2] = clamp(luma + 116130 * cb);
}
} // namespace smg
