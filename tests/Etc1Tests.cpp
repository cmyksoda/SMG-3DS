#include <smg/Etc1.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

smg::Image pattern(unsigned size, bool withAlpha) {
    smg::Image image;
    image.width = image.height = size;
    for (unsigned y = 0; y < size; ++y) for (unsigned x = 0; x < size; ++x) {
        const unsigned r = x * 255 / (size - 1), g = y * 255 / (size - 1), b = (x ^ y) & 0x40 ? 200 : 40;
        const unsigned a = withAlpha ? (x + y) * 255 / (2 * size - 2) : 255;
        image.pixels.push_back(r << 24 | g << 16 | b << 8 | a);
    }
    return image;
}

double psnr(const smg::Image& a, const smg::Image& b) {
    double sum = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        for (int shift : {24, 16, 8}) {
            const double d = double(a.pixels[i] >> shift & 255) - double(b.pixels[i] >> shift & 255);
            sum += d * d;
        }
    const double mse = sum / (a.pixels.size() * 3.0);
    return mse == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}
} // namespace

int runEtc1Tests() {
    smg::Image flat;
    flat.width = flat.height = 8;
    flat.pixels.assign(64, 0x884422ffu);
    auto bytes = smg::encodeEtc1(flat, false);
    check(bytes.size() == 32, "four 8-byte blocks per 8x8 tile");
    auto back = smg::decodeEtc1(bytes.data(), 8, 8, false);
    check(psnr(flat, back) > 40, "a flat color survives almost exactly");

    const auto gradient = pattern(64, false);
    bytes = smg::encodeEtc1(gradient, false);
    back = smg::decodeEtc1(bytes.data(), 64, 64, false);
    check(bytes.size() == 64 * 64 / 2, "four bits per pixel");
    check(psnr(gradient, back) > 30, "gradients keep block-compression quality");

    const auto translucent = pattern(32, true);
    bytes = smg::encodeEtc1(translucent, true);
    back = smg::decodeEtc1(bytes.data(), 32, 32, true);
    check(bytes.size() == 32 * 32, "alpha adds a word per block");
    bool alphaClose = true;
    for (std::size_t i = 0; i < back.pixels.size(); ++i)
        alphaClose &= std::abs(int(back.pixels[i] & 255) - int(translucent.pixels[i] & 255)) <= 9;
    check(alphaClose, "alpha kept to four bits");

    // A hand-built block: individual mode, red 15 over green 0, table 0, every pixel index 0.
    std::uint8_t tile[32]{};
    const std::uint64_t word = std::uint64_t(15) << 60;
    for (int i = 0; i < 8; ++i) tile[8 + i] = std::uint8_t(word >> (8 * i));
    back = smg::decodeEtc1(tile, 8, 8, false);
    check(back.pixels[4] >> 24 == 255 && (back.pixels[0] >> 24) == 2, "second block is the tile's top right, stored little-endian");
    std::printf("%u ETC1 checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runEtc1Tests(); }
#endif
