#pragma once
#include <smg/Aes.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace smg {
// A Wii disc image, ISO or WBFS, read as the game reads its disc: the data partition's bytes, decrypted a cluster
// (0x8000 bytes, 0x7C00 of them data) at a time.
class WiiDisc {
public:
    // `size` bytes of the image file from `offset`; false when they can't be read.
    using Reader = std::function<bool(std::uint64_t offset, void* out, std::uint32_t size)>;
    // Finds the data partition and its title key, which `commonKey` decrypts from the ticket (or `titleKey`, given
    // already decrypted), then reads boot.bin and the file table.
    bool open(Reader reader, const std::uint8_t* commonKey, const std::uint8_t* titleKey, std::string& error);
    // The data partition's bytes from `offset`, the offsets its file table gives (shifted back up by two).
    bool read(std::uint64_t offset, void* out, std::uint32_t size);
    std::string gameId() const { return std::string(boot_, boot_ + 6); }
    const std::vector<std::uint8_t>& fileTable() const { return fileTable_; }
    bool isWbfs() const { return !blocks_.empty(); }

private:
    bool readImage(std::uint64_t offset, void* out, std::uint32_t size);  // disc offsets, through WBFS's block map
    bool openWbfs(std::string& error);
    bool cluster(std::uint64_t index);

    Reader reader_;
    std::vector<std::uint16_t> blocks_;  // WBFS: each disc block's place in the file (0: not stored, zeros)
    unsigned blockShift_ = 0;
    std::uint64_t data_ = 0, dataSize_ = 0;  // the partition's encrypted data area on the disc
    std::unique_ptr<Aes128> aes_;
    std::uint8_t boot_[0x440] = {};
    std::vector<std::uint8_t> fileTable_;
    std::vector<std::uint8_t> raw_, plain_;  // the last cluster, as stored and decrypted
    std::uint64_t cached_ = ~std::uint64_t(0);
};
} // namespace smg
