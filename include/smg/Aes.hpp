#pragma once
#include <cstddef>
#include <cstdint>

namespace smg {
// AES-128 in CBC mode, as a Wii disc's partitions are encrypted: decryption by tables (about twenty instructions a byte
// on the ARM11), encryption for tests and tools.
class Aes128 {
public:
    explicit Aes128(const std::uint8_t key[16]);
    // `size` a multiple of sixteen; `in` and `out` may be the same.
    void decryptCbc(const std::uint8_t iv[16], const std::uint8_t* in, std::uint8_t* out, std::size_t size) const;
    void encryptCbc(const std::uint8_t iv[16], const std::uint8_t* in, std::uint8_t* out, std::size_t size) const;

private:
    void decryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const;
    void encryptBlock(const std::uint8_t in[16], std::uint8_t out[16]) const;
    std::uint32_t encrypt_[44], decrypt_[44];
};
} // namespace smg
