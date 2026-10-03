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
std::uint64_t big64(const std::uint8_t* p) { return std::uint64_t(big32(p)) << 32 | big32(p + 4); }
std::uint16_t big16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }

// RVZ's junk, the Wii's padding regenerated: a lagged Fibonacci generator (j 32, k 521) seeded by 17 words, whose words
// are given out big-endian with the third byte from bits 18-25 rather than 16-23 (folded into the words here).
class Junk {
public:
    Junk(const std::uint8_t seed[68], std::uint64_t skip) {
        for (int i = 0; i < 17; ++i) words_[i] = big32(seed + 4 * i);
        for (int i = 17; i < K; ++i) words_[i] = words_[i - 17] << 23 ^ words_[i - 16] >> 9 ^ words_[i - 1];
        for (std::uint32_t& w : words_) w = (w & 0xFF00FFFF) | (w >> 2 & 0x00FF0000);
        for (int i = 0; i < 4; ++i) forward();
        for (; skip >= K * 4; skip -= K * 4) forward();
        position_ = static_cast<std::size_t>(skip);
    }
    void fill(std::uint8_t* out, std::uint64_t count) {
        for (; count; --count) {
            *out++ = static_cast<std::uint8_t>(words_[position_ / 4] >> (24 - 8 * (position_ % 4)));
            if (++position_ == K * 4) forward(), position_ = 0;
        }
    }

private:
    static constexpr int J = 32, K = 521;
    void forward() {
        for (int i = 0; i < J; ++i) words_[i] ^= words_[i + K - J];
        for (int i = J; i < K; ++i) words_[i] ^= words_[i - J];
    }
    std::uint32_t words_[K];
    std::size_t position_ = 0;
};

// A packed group's data: runs, each a big-endian size, its top bit set for junk (then the seed) or clear for literal
// bytes. Junk is seeded at its place in the stretch, counted in 0x8000s even where the hashes are left out.
bool unpack(const std::uint8_t* p, std::size_t size, std::uint8_t* out, std::uint64_t bytes, std::uint64_t dataOffset) {
    std::size_t at = 0;
    for (std::uint64_t made = 0; made < bytes;) {
        if (at + 4 > size) return false;
        const std::uint32_t run = big32(p + at);
        at += 4;
        const std::uint64_t take = std::min<std::uint64_t>(run & 0x7FFFFFFF, bytes - made);
        if (run >> 31) {
            if (at + 68 > size) return false;
            Junk(p + at, (dataOffset + made) % ClusterBytes).fill(out + made, take);
            at += 68;
        } else {
            if (at + take > size) return false;
            std::memcpy(out + made, p + at, static_cast<std::size_t>(take));
            at += static_cast<std::size_t>(take);
        }
        made += take;
    }
    return true;
}
} // namespace

bool WiiDisc::readImage(std::uint64_t offset, void* out, std::uint32_t size) {
    auto* to = static_cast<std::uint8_t*>(out);
    if (rvz_) {
        if (offset < sizeof rvzHeader_) {
            const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, sizeof rvzHeader_ - offset));
            std::memcpy(to, rvzHeader_ + offset, take);
            to += take, offset += take, size -= take;
        }
        while (size) {
            const auto stretch = std::find_if(rvzRaw_.begin(), rvzRaw_.end(), [&](const RvzStretch& s) { return offset >= s.start && offset < s.start + s.size; });
            if (stretch == rvzRaw_.end() || !rvzRead(*stretch, rvzChunk_, 0, offset, to, size)) return false;
        }
        return true;
    }
    if (blocks_.empty()) return reader_(offset, out, size);
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

// An RVZ file: its own header, then a second holding the disc's first 0x80 bytes, the chunk size, the compression and
// where its tables lie: the partitions (each the stretches of its data), the raw stretches (the rest of the disc) and
// the groups. Its hashes, which only check the data, are not checked here.
bool WiiDisc::openRvz(std::string& error) {
    std::uint8_t head[0x48 + 0xD4];
    if (!reader_(0, head, sizeof head)) return error = "the RVZ header could not be read", false;
    if (big32(head + 8) > 0x01000000) return error = "the RVZ is a newer version than this reader knows", false;
    if (big32(head + 0xC) < 0xD4) return error = "the RVZ's second header is too short", false;
    const std::uint8_t* h = head + 0x48;
    const std::uint32_t compression = big32(h + 4);
    if (compression != 0 && compression != 5) return error = "the RVZ is compressed other than with Zstandard", false;
    rvzCompressed_ = compression == 5;
    if (rvzCompressed_ && !decompress_) return error = "the RVZ needs Zstandard, which this build has none of", false;
    rvzChunk_ = big32(h + 0xC);
    if (rvzChunk_ < ClusterBytes || ((rvzChunk_ & (rvzChunk_ - 1)) && rvzChunk_ % 0x200000)) return error = "the RVZ's chunk size is not an RVZ's", false;
    std::memcpy(rvzHeader_, h + 0x10, sizeof rvzHeader_);
    const std::uint32_t partitions = big32(h + 0x90), partitionSize = big32(h + 0x94), raws = big32(h + 0xB4), groups = big32(h + 0xC4);
    if (partitions > 16 || partitionSize < 0x30 || raws > 1024 || groups > (1u << 22)) return error = "the RVZ's tables are not an RVZ's", false;
    // The raw and group tables are compressed as the groups are.
    const auto table = [&](std::uint64_t at, std::uint32_t stored, std::size_t size, std::vector<std::uint8_t>& out) {
        std::vector<std::uint8_t> in(stored);
        if (!reader_(at, in.data(), stored)) return false;
        if (!rvzCompressed_) return in.size() >= size && (out = std::move(in), true);
        out.resize(size);
        return decompress_(in.data(), in.size(), out.data(), out.size()) == size;
    };
    std::vector<std::uint8_t> partitionTable(std::size_t(partitions) * partitionSize), rawTable, groupTable;
    if (!reader_(big64(h + 0x98), partitionTable.data(), std::uint32_t(partitionTable.size())) ||
        !table(big64(h + 0xB8), big32(h + 0xC0), std::size_t(raws) * 0x18, rawTable) || !table(big64(h + 0xC8), big32(h + 0xD0), std::size_t(groups) * 12, groupTable))
        return error = "the RVZ's tables could not be read", false;
    rvzPartitions_.assign(partitions, {}), rvzPartitionAt_.assign(partitions, 0);
    for (std::uint32_t i = 0; i < partitions; ++i) {
        const std::uint8_t* entry = &partitionTable[std::size_t(i) * partitionSize + 16];
        const std::uint32_t first = big32(entry);
        rvzPartitionAt_[i] = std::uint64_t(first) * ClusterBytes;
        for (int k = 0; k < 2; ++k, entry += 16)
            if (big32(entry + 4))
                rvzPartitions_[i].push_back({std::uint64_t(big32(entry) - first) * ClusterData, std::uint64_t(big32(entry + 4)) * ClusterData, big32(entry + 8), big32(entry + 12)});
    }
    // The first raw stretch says it starts at 0x80 but holds the disc from 0: each starts at its 0x8000 boundary.
    rvzRaw_.clear();
    for (std::uint32_t i = 0; i < raws; ++i) {
        const std::uint8_t* entry = &rawTable[std::size_t(i) * 0x18];
        const std::uint64_t start = big64(entry), skipped = start % ClusterBytes;
        if (big64(entry + 8)) rvzRaw_.push_back({start - skipped, big64(entry + 8) + skipped, big32(entry + 16), big32(entry + 20)});
    }
    rvzGroups_.resize(groups);
    for (std::uint32_t i = 0; i < groups; ++i) rvzGroups_[i] = {big32(&groupTable[12 * i]), big32(&groupTable[12 * i + 4]), big32(&groupTable[12 * i + 8])};
    rvz_ = true;
    return true;
}

bool WiiDisc::rvzRead(const RvzStretch& stretch, std::uint64_t chunk, unsigned lists, std::uint64_t& offset, std::uint8_t*& to, std::uint32_t& size) {
    while (size && offset < stretch.start + stretch.size) {
        const std::uint64_t index = (offset - stretch.start) / chunk, within = (offset - stretch.start) % chunk;
        if (index >= stretch.groups) return false;
        const std::uint64_t bytes = std::min(chunk, stretch.size - index * chunk);
        if (!rvzGroup(stretch.firstGroup + static_cast<std::uint32_t>(index), bytes, lists, index * chunk)) return false;
        const auto take = static_cast<std::uint32_t>(std::min<std::uint64_t>(size, bytes - within));
        std::memcpy(to, rvzGroupData_.data() + within, take);
        to += take, offset += take, size -= take;
    }
    return true;
}

// A group's `bytes` of data: stored or compressed (the size's top bit), past the hash exception lists a partition's
// groups begin with (they only rebuild the hashes, which this reader leaves out), as they are or packed.
bool WiiDisc::rvzGroup(std::uint32_t index, std::uint64_t bytes, unsigned lists, std::uint64_t dataOffset) {
    if (index == rvzCached_) return true;
    if (index >= rvzGroups_.size()) return false;
    rvzCached_ = ~std::uint32_t(0);
    const RvzGroup& group = rvzGroups_[index];
    rvzGroupData_.resize(static_cast<std::size_t>(bytes));
    const std::uint32_t stored = group.size & 0x7FFFFFFF;
    if (!stored) {
        std::fill(rvzGroupData_.begin(), rvzGroupData_.end(), 0);
        return rvzCached_ = index, true;
    }
    rvzIn_.resize(stored);
    if (!reader_(std::uint64_t(group.offset4) << 2, rvzIn_.data(), stored)) return false;
    const std::uint8_t* p = rvzIn_.data();
    std::size_t size = stored;
    const bool compressed = group.size >> 31 && rvzCompressed_;
    if (compressed) {
        constexpr std::size_t MostPerList = 2 + 22 * 52 * 64;  // an exception for each 20 bytes of a cluster's hashes
        rvzOut_.resize(lists * MostPerList + (group.packedSize ? group.packedSize : static_cast<std::size_t>(bytes)));
        size = decompress_(p, stored, rvzOut_.data(), rvzOut_.size());
        if (size == SIZE_MAX) return false;
        p = rvzOut_.data();
    }
    std::size_t at = 0;
    for (unsigned list = 0; list < lists; ++list) {
        if (at + 2 > size) return false;
        at += 2 + 22 * std::size_t(big16(p + at));
    }
    if (lists && !compressed) at = (at + 3) & ~std::size_t(3);  // stored lists end on a word
    if (at > size) return false;
    p += at, size -= at;
    if (group.packedSize) {
        if (!unpack(p, std::min<std::size_t>(size, group.packedSize), rvzGroupData_.data(), bytes, dataOffset)) return false;
    } else {
        if (size < bytes) return false;
        std::memcpy(rvzGroupData_.data(), p, static_cast<std::size_t>(bytes));
    }
    rvzCached_ = index;
    return true;
}

bool WiiDisc::open(Reader reader, const std::uint8_t* commonKey, const std::uint8_t* titleKey, std::string& error) {
    reader_ = std::move(reader);
    blocks_.clear(), cached_ = ~std::uint64_t(0), rvz_ = false, rvzData_ = nullptr, rvzCached_ = ~std::uint32_t(0);
    std::uint8_t magic[4];
    if (!reader_(0, magic, 4)) return error = "the image could not be read", false;
    if (!std::memcmp(magic, "WBFS", 4) && !openWbfs(error)) return false;
    if (!std::memcmp(magic, "RVZ\x01", 4) && !openRvz(error)) return false;
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
    data_ = partition + (std::uint64_t(big32(ticket + 0x2B8)) << 2);
    dataSize_ = std::uint64_t(big32(ticket + 0x2BC)) << 2;
    if (rvz_) {
        // Its data is kept decrypted, by where it begins on the disc.
        for (std::size_t i = 0; i < rvzPartitions_.size(); ++i)
            if (rvzPartitionAt_[i] == data_) rvzData_ = &rvzPartitions_[i];
        if (!rvzData_) return error = "the RVZ holds no data for the disc's data partition", false;
    } else {
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
    }
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
    if (rvz_) {
        const std::uint64_t chunk = std::uint64_t(rvzChunk_) / ClusterBytes * ClusterData;
        const auto lists = static_cast<unsigned>(std::max<std::uint64_t>(1, chunk / (std::uint64_t(ClusterData) * 64)));
        for (const RvzStretch& stretch : *rvzData_)
            if (size && offset >= stretch.start && !rvzRead(stretch, chunk, lists, offset, to, size)) return false;
        return size == 0;
    }
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
