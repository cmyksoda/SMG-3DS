#include <smg/WiiDisc.hpp>
#include <algorithm>
#include <cstring>

namespace smg {
namespace {
constexpr std::uint32_t ClusterBytes = 0x8000, ClusterData = 0x7C00, ClusterHashes = 0x400;
constexpr std::uint32_t WiiMagic = 0x5D1C9EA3;  // at 0x18 of a Wii disc's header
// The largest disc a WBFS maps: a dual-layer disc's 143,432 sectors per layer.
constexpr std::uint64_t WbfsDiscBytes = std::uint64_t(ClusterBytes) * 143432 * 2;

std::uint32_t big32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
} // namespace

bool WiiDisc::readImage(std::uint64_t offset, void* out, std::uint32_t size) {
    if (blocks_.empty()) return reader_(offset, out, size);
    auto* to = static_cast<std::uint8_t*>(out);
    const std::uint64_t blockBytes = std::uint64_t(1) << blockShift_;
    while (size) {
        const std::uint64_t block = offset >> blockShift_, within = offset & (blockBytes - 1);
        const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, blockBytes - within));
        const std::uint16_t stored = block < blocks_.size() ? blocks_[block] : 0;
        if (!stored) std::memset(to, 0, take);  // a block the dump left out holds nothing the game reads
        else if (!reader_((std::uint64_t(stored) << blockShift_) + within, to, take)) return false;
        to += take, offset += take, size -= take;
    }
    return true;
}

// A WBFS file: a header sector (sector size, block size, a byte per disc slot), then each disc's copy of its header
// and a big-endian table of where each of its blocks is stored.
bool WiiDisc::openWbfs(std::string& error) {
    std::uint8_t header[12];
    if (!reader_(0, header, sizeof header)) return error = "the WBFS header could not be read", false;
    const unsigned sectorShift = header[8];
    blockShift_ = header[9];
    if (sectorShift < 9 || sectorShift > 12 || blockShift_ < 15 || blockShift_ > 30) return error = "the WBFS header's sizes are not a WBFS's", false;
    const std::uint32_t sectorBytes = 1u << sectorShift;
    std::vector<std::uint8_t> slots(sectorBytes - 12);
    if (!reader_(12, slots.data(), std::uint32_t(slots.size()))) return error = "the WBFS disc table could not be read", false;
    const auto used = std::find_if(slots.begin(), slots.end(), [](std::uint8_t s) { return s != 0; });
    if (used == slots.end()) return error = "the WBFS holds no disc", false;
    const std::uint64_t perDisc = WbfsDiscBytes >> blockShift_;
    const std::uint64_t infoBytes = (0x100 + perDisc * 2 + sectorBytes - 1) / sectorBytes * sectorBytes;
    std::vector<std::uint8_t> table(std::size_t(perDisc) * 2);
    if (!reader_(sectorBytes + std::uint64_t(used - slots.begin()) * infoBytes + 0x100, table.data(), std::uint32_t(table.size())))
        return error = "the WBFS block table could not be read", false;
    blocks_.resize(std::size_t(perDisc));
    for (std::size_t i = 0; i < blocks_.size(); ++i) blocks_[i] = static_cast<std::uint16_t>(table[2 * i] << 8 | table[2 * i + 1]);
    return true;
}

bool WiiDisc::open(Reader reader, const std::uint8_t* commonKey, const std::uint8_t* titleKey, std::string& error) {
    reader_ = std::move(reader);
    blocks_.clear(), cached_ = ~std::uint64_t(0);
    std::uint8_t magic[4];
    if (!reader_(0, magic, 4)) return error = "the image could not be read", false;
    if (!std::memcmp(magic, "WBFS", 4) && !openWbfs(error)) return false;
    std::uint8_t header[0x20];
    if (!readImage(0, header, sizeof header)) return error = "the disc header could not be read", false;
    if (big32(header + 0x18) != WiiMagic) return error = "the image is not a Wii disc", false;
    // Four partition tables at 0x40000, each a count and where its entries are (offset and type, both words).
    std::uint8_t tables[32];
    if (!readImage(0x40000, tables, sizeof tables)) return error = "the partition tables could not be read", false;
    std::uint64_t partition = 0;
    for (int t = 0; t < 4 && !partition; ++t) {
        const std::uint32_t count = big32(tables + 8 * t);
        const std::uint64_t at = std::uint64_t(big32(tables + 8 * t + 4)) << 2;
        for (std::uint32_t i = 0; i < count && i < 64 && !partition; ++i) {
            std::uint8_t entry[8];
            if (!readImage(at + 8 * i, entry, 8)) return error = "a partition entry could not be read", false;
            if (big32(entry + 4) == 0) partition = std::uint64_t(big32(entry)) << 2;  // type 0: the game's data
        }
    }
    if (!partition) return error = "the disc has no data partition", false;
    // The partition begins with its ticket (the title key, encrypted under the common key with the title ID as IV),
    // then where its data lies.
    std::uint8_t ticket[0x2C0];
    if (!readImage(partition, ticket, sizeof ticket)) return error = "the partition's ticket could not be read", false;
    std::uint8_t key[16];
    if (titleKey) {
        std::memcpy(key, titleKey, 16);
    } else {
        if (!commonKey) return error = "no key to decrypt the partition's title key with", false;
        if (ticket[0x1F1] != 0) return error = "the title key is under another common key (a Korean disc)", false;
        std::uint8_t iv[16] = {};
        std::memcpy(iv, ticket + 0x1DC, 8);
        Aes128(commonKey).decryptCbc(iv, ticket + 0x1BF, key, 16);
    }
    aes_ = std::make_unique<Aes128>(key);
    data_ = partition + (std::uint64_t(big32(ticket + 0x2B8)) << 2);
    dataSize_ = std::uint64_t(big32(ticket + 0x2BC)) << 2;
    raw_.resize(ClusterBytes), plain_.resize(ClusterData);
    if (!read(0, boot_, sizeof boot_)) return error = "the partition's boot.bin could not be read", false;
    // A wrong key decrypts to noise: boot.bin's own Wii magic says the key was right.
    if (big32(boot_ + 0x18) != WiiMagic) return error = "the partition did not decrypt (a wrong key?)", false;
    const std::uint64_t tableAt = std::uint64_t(big32(boot_ + 0x424)) << 2, tableSize = std::uint64_t(big32(boot_ + 0x428)) << 2;
    if (!tableSize || tableSize > (64u << 20)) return error = "the file table's size is not a file table's", false;
    fileTable_.resize(std::size_t(tableSize));
    if (!read(tableAt, fileTable_.data(), std::uint32_t(tableSize))) return error = "the file table could not be read", false;
    return true;
}

bool WiiDisc::cluster(std::uint64_t index) {
    if (index == cached_) return true;
    if (std::uint64_t(index) * ClusterBytes >= dataSize_) return false;
    if (!readImage(data_ + index * ClusterBytes, raw_.data(), ClusterBytes)) return false;
    // The data's IV is the last sixteen bytes of the H2 hashes, as stored (encrypted).
    aes_->decryptCbc(raw_.data() + 0x3D0, raw_.data() + ClusterHashes, plain_.data(), ClusterData);
    cached_ = index;
    return true;
}

bool WiiDisc::read(std::uint64_t offset, void* out, std::uint32_t size) {
    auto* to = static_cast<std::uint8_t*>(out);
    while (size) {
        const std::uint64_t index = offset / ClusterData;
        const auto within = static_cast<std::uint32_t>(offset % ClusterData);
        if (!cluster(index)) return false;
        const std::uint32_t take = std::min(size, ClusterData - within);
        std::memcpy(to, plain_.data() + within, take);
        to += take, offset += take, size -= take;
    }
    return true;
}
} // namespace smg
