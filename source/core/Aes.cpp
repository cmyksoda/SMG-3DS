#include <smg/Aes.hpp>
#include <cstring>

namespace smg {
namespace {
std::uint8_t times2(std::uint8_t x) { return static_cast<std::uint8_t>(x << 1 ^ (x & 0x80 ? 0x1B : 0)); }
std::uint8_t multiply(std::uint8_t a, std::uint8_t b) {
    std::uint8_t product = 0;
    for (; b; b >>= 1, a = times2(a))
        if (b & 1) product ^= a;
    return product;
}
std::uint8_t rotate(std::uint8_t x, int bits) { return static_cast<std::uint8_t>(x << bits | x >> (8 - bits)); }
std::uint32_t rotateRight(std::uint32_t x, int bits) { return x >> bits | x << (32 - bits); }

// The S-boxes from the field's inverses (powers of 3 reach every non-zero element), and the round tables from them.
struct Tables {
    std::uint8_t sbox[256], inverse[256];
    std::uint32_t te[4][256], td[4][256];
    Tables() {
        std::uint8_t power[255], log[256] = {};
        std::uint8_t x = 1;
        for (int i = 0; i < 255; ++i) power[i] = x, log[x] = static_cast<std::uint8_t>(i), x ^= times2(x);
        for (int i = 0; i < 256; ++i) {
            const std::uint8_t inv = i ? power[(255 - log[i]) % 255] : 0;
            const auto s = static_cast<std::uint8_t>(inv ^ rotate(inv, 1) ^ rotate(inv, 2) ^ rotate(inv, 3) ^ rotate(inv, 4) ^ 0x63);
            sbox[i] = s, inverse[s] = static_cast<std::uint8_t>(i);
        }
        for (int i = 0; i < 256; ++i) {
            const std::uint8_t s = sbox[i], v = inverse[i];
            te[0][i] = std::uint32_t(multiply(s, 2)) << 24 | std::uint32_t(s) << 16 | std::uint32_t(s) << 8 | multiply(s, 3);
            td[0][i] = std::uint32_t(multiply(v, 14)) << 24 | std::uint32_t(multiply(v, 9)) << 16 | std::uint32_t(multiply(v, 13)) << 8 | multiply(v, 11);
            for (int t = 1; t < 4; ++t) te[t][i] = rotateRight(te[0][i], 8 * t), td[t][i] = rotateRight(td[0][i], 8 * t);
        }
    }
};
const Tables& tables() {
    static const Tables instance;
    return instance;
}

std::uint32_t load(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
void store(std::uint32_t v, std::uint8_t* p) { p[0] = std::uint8_t(v >> 24), p[1] = std::uint8_t(v >> 16), p[2] = std::uint8_t(v >> 8), p[3] = std::uint8_t(v); }
} // namespace

Aes128::Aes128(const std::uint8_t key[16]) {
    const Tables& t = tables();
    const auto substitute = [&](std::uint32_t w) {
        return std::uint32_t(t.sbox[w >> 24]) << 24 | std::uint32_t(t.sbox[w >> 16 & 255]) << 16 | std::uint32_t(t.sbox[w >> 8 & 255]) << 8 |
               t.sbox[w & 255];
    };
    for (int i = 0; i < 4; ++i) encrypt_[i] = load(key + 4 * i);
    std::uint8_t rcon = 1;
    for (int i = 4; i < 44; ++i) {
        std::uint32_t w = encrypt_[i - 1];
        if (i % 4 == 0) w = substitute(w << 8 | w >> 24) ^ std::uint32_t(rcon) << 24, rcon = times2(rcon);
        encrypt_[i] = encrypt_[i - 4] ^ w;
    }
    // The equivalent inverse cipher's keys: the rounds in reverse, the middle ones through InvMixColumns.
    for (int round = 0; round <= 10; ++round)
        for (int c = 0; c < 4; ++c) {
            const std::uint32_t w = encrypt_[4 * (10 - round) + c];
            decrypt_[4 * round + c] = round == 0 || round == 10 ? w
                                                                : t.td[0][t.sbox[w >> 24]] ^ t.td[1][t.sbox[w >> 16 & 255]] ^
                                                                      t.td[2][t.sbox[w >> 8 & 255]] ^ t.td[3][t.sbox[w & 255]];
        }
}

void Aes128::encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const Tables& t = tables();
    const std::uint32_t* k = encrypt_;
    std::uint32_t s0 = load(in) ^ k[0], s1 = load(in + 4) ^ k[1], s2 = load(in + 8) ^ k[2], s3 = load(in + 12) ^ k[3];
    for (int round = 1; round < 10; ++round) {
        k += 4;
        const std::uint32_t t0 = t.te[0][s0 >> 24] ^ t.te[1][s1 >> 16 & 255] ^ t.te[2][s2 >> 8 & 255] ^ t.te[3][s3 & 255] ^ k[0];
        const std::uint32_t t1 = t.te[0][s1 >> 24] ^ t.te[1][s2 >> 16 & 255] ^ t.te[2][s3 >> 8 & 255] ^ t.te[3][s0 & 255] ^ k[1];
        const std::uint32_t t2 = t.te[0][s2 >> 24] ^ t.te[1][s3 >> 16 & 255] ^ t.te[2][s0 >> 8 & 255] ^ t.te[3][s1 & 255] ^ k[2];
        const std::uint32_t t3 = t.te[0][s3 >> 24] ^ t.te[1][s0 >> 16 & 255] ^ t.te[2][s1 >> 8 & 255] ^ t.te[3][s2 & 255] ^ k[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    k += 4;
    const auto last = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t key) {
        return (std::uint32_t(t.sbox[a >> 24]) << 24 | std::uint32_t(t.sbox[b >> 16 & 255]) << 16 | std::uint32_t(t.sbox[c >> 8 & 255]) << 8 |
                t.sbox[d & 255]) ^ key;
    };
    store(last(s0, s1, s2, s3, k[0]), out), store(last(s1, s2, s3, s0, k[1]), out + 4);
    store(last(s2, s3, s0, s1, k[2]), out + 8), store(last(s3, s0, s1, s2, k[3]), out + 12);
}

void Aes128::decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const {
    const Tables& t = tables();
    const std::uint32_t* k = decrypt_;
    std::uint32_t s0 = load(in) ^ k[0], s1 = load(in + 4) ^ k[1], s2 = load(in + 8) ^ k[2], s3 = load(in + 12) ^ k[3];
    for (int round = 1; round < 10; ++round) {
        k += 4;
        const std::uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][s3 >> 16 & 255] ^ t.td[2][s2 >> 8 & 255] ^ t.td[3][s1 & 255] ^ k[0];
        const std::uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][s0 >> 16 & 255] ^ t.td[2][s3 >> 8 & 255] ^ t.td[3][s2 & 255] ^ k[1];
        const std::uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][s1 >> 16 & 255] ^ t.td[2][s0 >> 8 & 255] ^ t.td[3][s3 & 255] ^ k[2];
        const std::uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][s2 >> 16 & 255] ^ t.td[2][s1 >> 8 & 255] ^ t.td[3][s0 & 255] ^ k[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    k += 4;
    const auto last = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t key) {
        return (std::uint32_t(t.inverse[a >> 24]) << 24 | std::uint32_t(t.inverse[b >> 16 & 255]) << 16 |
                std::uint32_t(t.inverse[c >> 8 & 255]) << 8 | t.inverse[d & 255]) ^ key;
    };
    store(last(s0, s3, s2, s1, k[0]), out), store(last(s1, s0, s3, s2, k[1]), out + 4);
    store(last(s2, s1, s0, s3, k[2]), out + 8), store(last(s3, s2, s1, s0, k[3]), out + 12);
}

void Aes128::decryptCbc(const std::uint8_t iv[16], const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
    std::uint8_t chain[16], next[16], block[16];
    std::memcpy(chain, iv, 16);
    for (std::size_t at = 0; at + 16 <= size; at += 16) {
        std::memcpy(next, in + at, 16);
        decryptBlock(next, block);
        for (int i = 0; i < 16; ++i) out[at + i] = static_cast<std::uint8_t>(block[i] ^ chain[i]);
        std::memcpy(chain, next, 16);
    }
}

void Aes128::encryptCbc(const std::uint8_t iv[16], const std::uint8_t* in, std::uint8_t* out, std::size_t size) const {
    std::uint8_t chain[16];
    std::memcpy(chain, iv, 16);
    for (std::size_t at = 0; at + 16 <= size; at += 16) {
        for (int i = 0; i < 16; ++i) chain[i] = static_cast<std::uint8_t>(chain[i] ^ in[at + i]);
        encryptBlock(chain, chain);
        std::memcpy(out + at, chain, 16);
    }
}
} // namespace smg
