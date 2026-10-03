#pragma once
#include <smg/Aes.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace smg {
// A Wii disc image, ISO, WBFS or RVZ, read as the game reads its disc: the data partition's bytes, decrypted a cluster
// (0x8000 bytes, 0x7C00 of them data) at a time, or for RVZ, which keeps them decrypted, a group at a time.
class WiiDisc {
public:
    // `size` bytes of the image file from `offset`; false when they can't be read.
    using Reader = std::function<bool(std::uint64_t offset, void* out, std::uint32_t size)>;
    // RVZ's compression (Zstandard): `size` bytes at `in` into at most `capacity` at `out`. The bytes made, or SIZE_MAX.
    using Decompress = std::function<std::size_t(const void* in, std::size_t size, void* out, std::size_t capacity)>;
    void decompressWith(Decompress decompress) { decompress_ = std::move(decompress); }
    // Finds the data partition and its title key, which `commonKey` decrypts from the ticket (or `titleKey`, given
    // already decrypted), then reads boot.bin and the file table. An RVZ needs neither key.
    bool open(Reader reader, const std::uint8_t* commonKey, const std::uint8_t* titleKey, std::string& error);
    // The data partition's bytes from `offset`, the offsets its file table gives (shifted back up by two).
    bool read(std::uint64_t offset, void* out, std::uint32_t size);
    std::string gameId() const { return std::string(boot_, boot_ + 6); }
    const std::uint8_t* bootBin() const { return boot_; }  // the partition's first 0x440 bytes
    const std::vector<std::uint8_t>& fileTable() const { return fileTable_; }
    const char* format() const { return rvz_ ? "RVZ" : !blocks_.empty() ? "WBFS" : "ISO"; }

private:
    bool readImage(std::uint64_t offset, void* out, std::uint32_t size);  // disc offsets, through WBFS's block map
    bool openWbfs(std::string& error);
    bool cluster(std::uint64_t index);

    // RVZ: the disc in stretches of groups, each group `chunk` bytes of data (the last of a stretch fewer), compressed or
    // stored, its junk (the Wii's padding) given as seeds; a partition's groups keep its data without the hash blocks.
    struct RvzStretch {
        std::uint64_t start = 0, size = 0;  // raw: disc offsets; partition: offsets in its data
        std::uint32_t firstGroup = 0, groups = 0;
    };
    struct RvzGroup {
        std::uint32_t offset4 = 0, size = 0, packedSize = 0;
    };
    bool openRvz(std::string& error);
    bool rvzRead(const RvzStretch& stretch, std::uint64_t chunk, unsigned lists, std::uint64_t& offset, std::uint8_t*& to,
                 std::uint32_t& size);
    bool rvzGroup(std::uint32_t index, std::uint64_t bytes, unsigned lists, std::uint64_t dataOffset);

    Reader reader_;
    Decompress decompress_;
    std::vector<std::uint16_t> blocks_;  // WBFS: each disc block's place in the file (0: not stored, zeros)
    unsigned blockShift_ = 0;
    std::uint64_t data_ = 0, dataSize_ = 0;  // the partition's encrypted data area on the disc
    std::unique_ptr<Aes128> aes_;
    std::uint8_t boot_[0x440] = {};
    std::vector<std::uint8_t> fileTable_;
    std::vector<std::uint8_t> raw_, plain_;  // the last cluster, as stored and decrypted
    std::uint64_t cached_ = ~std::uint64_t(0);

    bool rvz_ = false;
    std::uint32_t rvzChunk_ = 0;
    bool rvzCompressed_ = false;
    std::uint8_t rvzHeader_[0x80] = {};
    std::vector<RvzStretch> rvzRaw_;
    std::vector<std::vector<RvzStretch>> rvzPartitions_;  // each partition's (one or two) stretches
    std::vector<std::uint64_t> rvzPartitionAt_;          // where each partition's data begins on the disc
    const std::vector<RvzStretch>* rvzData_ = nullptr;    // the data partition's
    std::vector<RvzGroup> rvzGroups_;
    std::vector<std::uint8_t> rvzIn_, rvzOut_, rvzGroupData_;
    std::uint32_t rvzCached_ = ~std::uint32_t(0);
};
} // namespace smg
