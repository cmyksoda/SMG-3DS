// AES-128 against FIPS-197's example, and a small Wii disc made here (encrypted as Nintendo's are) read back as an ISO,
// as a WBFS and as an RVZ.
#include <smg/Aes.hpp>
#include <smg/WiiDisc.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    if (!pass) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
    ++checks;
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    b[at] = std::uint8_t(v >> 24), b[at + 1] = std::uint8_t(v >> 16), b[at + 2] = std::uint8_t(v >> 8), b[at + 3] = std::uint8_t(v);
}
void put64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) { put32(b, at, std::uint32_t(v >> 32)), put32(b, at + 4, std::uint32_t(v)); }
std::uint32_t get32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }

void fipsExample() {
    std::uint8_t key[16], plain[16], cipher[16], back[16];
    for (int i = 0; i < 16; ++i) key[i] = std::uint8_t(i), plain[i] = std::uint8_t(i * 0x11);
    const std::uint8_t expected[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30, 0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
    const std::uint8_t zero[16] = {};
    smg::Aes128 aes(key);
    aes.encryptCbc(zero, plain, cipher, 16);  // one block with a zero IV is the bare cipher
    check(!std::memcmp(cipher, expected, 16), "AES-128 encrypts FIPS-197's example");
    aes.decryptCbc(zero, cipher, back, 16);
    check(!std::memcmp(back, plain, 16), "and decrypts it back");
}

// The partition's data as the game sees it: boot.bin, a file table with one file, the file's bytes past one cluster.
constexpr std::uint32_t FileAt = 0x9000, FileSize = 0x5000;
std::vector<std::uint8_t> partitionData() {
    std::vector<std::uint8_t> data(0x7C00 * 3, 0);
    std::memcpy(data.data(), "RMGE01", 6);
    put32(data, 0x18, 0x5D1C9EA3);
    const std::uint32_t tableAt = 0x2000;
    put32(data, 0x424, tableAt >> 2), put32(data, 0x428, 0x40 >> 2);
    data[tableAt] = 1, put32(data, tableAt + 8, 2);              // the root directory: two entries
    put32(data, tableAt + 12, 0), put32(data, tableAt + 16, FileAt >> 2), put32(data, tableAt + 20, FileSize);  // a file named ""
    for (std::uint32_t i = 0; i < FileSize; ++i) data[FileAt + i] = std::uint8_t(i * 7 + 3);
    return data;
}

// The image: header, one partition table, the partition (ticket and encrypted data clusters).
constexpr std::uint64_t Partition = 0x50000, DataOffset = 0x20000;
std::vector<std::uint8_t> discImage(const std::uint8_t common[16], const std::uint8_t title[16]) {
    const std::vector<std::uint8_t> plain = partitionData();
    const std::size_t clusters = plain.size() / 0x7C00;
    std::vector<std::uint8_t> disc(Partition + DataOffset + clusters * 0x8000, 0);
    std::memcpy(disc.data(), "RMGE01", 6);
    put32(disc, 0x18, 0x5D1C9EA3);
    put32(disc, 0x40000, 1), put32(disc, 0x40004, 0x40020 >> 2);  // table 0: one entry at 0x40020
    put32(disc, 0x40020, std::uint32_t(Partition >> 2)), put32(disc, 0x40024, 0);
    std::uint8_t* ticket = disc.data() + Partition;
    const std::uint8_t titleId[8] = {0, 1, 0, 0, 'R', 'M', 'G', 'E'};
    std::memcpy(ticket + 0x1DC, titleId, 8);
    std::uint8_t iv[16] = {};
    std::memcpy(iv, titleId, 8);
    smg::Aes128(common).encryptCbc(iv, title, ticket + 0x1BF, 16);
    put32(disc, Partition + 0x2B8, std::uint32_t(DataOffset >> 2)), put32(disc, Partition + 0x2BC, std::uint32_t(clusters * 0x8000 >> 2));
    const smg::Aes128 aes(title);
    for (std::size_t c = 0; c < clusters; ++c) {
        std::uint8_t* cluster = disc.data() + Partition + DataOffset + c * 0x8000;
        std::uint8_t hashes[0x400];
        for (int i = 0; i < 0x400; ++i) hashes[i] = std::uint8_t(c * 31 + i);
        const std::uint8_t zero[16] = {};
        aes.encryptCbc(zero, hashes, cluster, 0x400);
        aes.encryptCbc(cluster + 0x3D0, plain.data() + c * 0x7C00, cluster + 0x400, 0x7C00);
    }
    return disc;
}

// The same disc in a WBFS: 512-byte sectors, 32 KiB blocks, block i of the disc stored at block `first + i` when it
// holds anything.
std::vector<std::uint8_t> wbfsImage(const std::vector<std::uint8_t>& disc) {
    constexpr unsigned SectorShift = 9, BlockShift = 15;
    const std::size_t perDisc = std::size_t((std::uint64_t(0x8000) * 143432 * 2) >> BlockShift);
    const std::size_t infoBytes = (0x100 + perDisc * 2 + 511) / 512 * 512;
    const std::size_t first = (512 + infoBytes + (1u << BlockShift) - 1) >> BlockShift;
    const std::size_t blocks = (disc.size() + (1u << BlockShift) - 1) >> BlockShift;
    std::vector<std::uint8_t> wbfs((first + blocks) << BlockShift, 0);
    std::memcpy(wbfs.data(), "WBFS", 4);
    wbfs[8] = SectorShift, wbfs[9] = BlockShift, wbfs[12] = 1;  // slot 0 used
    std::memcpy(wbfs.data() + 512, disc.data(), 0x100);
    std::size_t stored = first;
    for (std::size_t b = 0; b < blocks; ++b) {
        const std::size_t from = b << BlockShift, size = std::min<std::size_t>(1u << BlockShift, disc.size() - from);
        bool empty = true;
        for (std::size_t i = 0; i < size && empty; ++i) empty = disc[from + i] == 0;
        if (empty) continue;  // left out, as a scrubbed dump does
        wbfs[512 + 0x100 + 2 * b] = std::uint8_t(stored >> 8), wbfs[512 + 0x100 + 2 * b + 1] = std::uint8_t(stored);
        std::memcpy(wbfs.data() + (stored << BlockShift), disc.data() + from, size);
        ++stored;
    }
    wbfs.resize(stored << BlockShift);
    return wbfs;
}

smg::WiiDisc::Reader readerOf(const std::vector<std::uint8_t>& image) {
    return [&image](std::uint64_t offset, void* out, std::uint32_t size) {
        if (offset + size > image.size()) return false;
        std::memcpy(out, image.data() + offset, size);
        return true;
    };
}

// The test's stand-in for Zstandard: every byte XORed.
std::vector<std::uint8_t> squeeze(std::vector<std::uint8_t> bytes) {
    for (std::uint8_t& b : bytes) b ^= 0xA5;
    return bytes;
}
std::size_t unsqueeze(const void* in, std::size_t size, void* out, std::size_t capacity) {
    if (size > capacity) return SIZE_MAX;
    for (std::size_t i = 0; i < size; ++i) static_cast<std::uint8_t*>(out)[i] = static_cast<const std::uint8_t*>(in)[i] ^ 0xA5;
    return size;
}

// RVZ's junk as its format's description gives it: 521 words from the seed's 17, advanced four times, each given out
// as its bits from 24, 18, 8 and 0.
std::vector<std::uint8_t> describedJunk(const std::uint8_t seed[68], std::size_t skip, std::size_t count) {
    std::uint32_t b[521];
    for (int i = 0; i < 17; ++i) b[i] = get32(seed + 4 * i);
    for (int i = 17; i < 521; ++i) b[i] = (b[i - 17] << 23) ^ (b[i - 16] >> 9) ^ b[i - 1];
    const auto advance = [&b] {
        for (int i = 0; i < 32; ++i) b[i] ^= b[i + 521 - 32];
        for (int i = 32; i < 521; ++i) b[i] ^= b[i - 32];
    };
    for (int i = 0; i < 4; ++i) advance();
    std::vector<std::uint8_t> out;
    for (int word = 0; out.size() < skip + count; ++word) {
        if (word == 521) advance(), word = 0;
        for (int shift : {24, 18, 8, 0}) out.push_back(std::uint8_t(b[word] >> shift));
    }
    return std::vector<std::uint8_t>(out.begin() + std::ptrdiff_t(skip), out.begin() + std::ptrdiff_t(skip + count));
}

// The same disc as an RVZ with 32 KiB chunks: one raw stretch (the disc up to the partition's data, empty blocks as
// empty groups), then the partition's three clusters of data as three groups, each stored another way: compressed with
// one hash exception; stored with none (its list padded to a word); compressed and packed as 0x100 literal bytes and
// junk. The disc's last cluster is that junk, as the RVZ gives it.
constexpr std::uint32_t JunkFrom = 2 * 0x7C00 + 0x100, JunkSize = 0x7C00 - 0x100;
std::vector<std::uint8_t> rvzImage(const std::vector<std::uint8_t>& disc, const std::uint8_t seed[68]) {
    const std::vector<std::uint8_t> plain = partitionData();
    constexpr std::uint64_t PartitionData = Partition + DataOffset;
    std::vector<std::vector<std::uint8_t>> groups;  // stored bytes
    std::vector<std::array<std::uint32_t, 2>> flags;  // size's top bit, packed size
    for (std::uint64_t at = 0; at < PartitionData; at += 0x8000) {
        std::vector<std::uint8_t> block(disc.begin() + std::ptrdiff_t(at), disc.begin() + std::ptrdiff_t(at + 0x8000));
        const bool empty = std::all_of(block.begin(), block.end(), [](std::uint8_t b) { return b == 0; });
        groups.push_back(empty ? std::vector<std::uint8_t>{} : squeeze(block)), flags.push_back({empty ? 0u : 0x80000000u, 0});
    }
    std::vector<std::uint8_t> a(2 + 22 + 0x7C00, 0x11);
    a[0] = 0, a[1] = 1;  // one exception
    std::copy(plain.begin(), plain.begin() + 0x7C00, a.begin() + 2 + 22);
    groups.push_back(squeeze(a)), flags.push_back({0x80000000u, 0});
    std::vector<std::uint8_t> b(4 + 0x7C00, 0);
    b[2] = b[3] = 0xEE;  // no exceptions, then padding to a word
    std::copy(plain.begin() + 0x7C00, plain.begin() + 0xF800, b.begin() + 4);
    groups.push_back(b), flags.push_back({0, 0});
    std::vector<std::uint8_t> packed(4 + 0x100 + 4 + 68);
    put32(packed, 0, 0x100);
    for (int i = 0; i < 0x100; ++i) packed[4 + i] = std::uint8_t(0xF800 + i);
    put32(packed, 4 + 0x100, 0x80000000u | JunkSize);
    std::copy(seed, seed + 68, packed.begin() + 4 + 0x100 + 4);
    std::vector<std::uint8_t> c(2 + packed.size(), 0);
    std::copy(packed.begin(), packed.end(), c.begin() + 2);
    groups.push_back(squeeze(c)), flags.push_back({0x80000000u, std::uint32_t(packed.size())});
    const std::uint32_t rawGroups = std::uint32_t(PartitionData / 0x8000);

    std::vector<std::uint8_t> partitions(0x30, 0);
    put32(partitions, 16, rawGroups), put32(partitions, 20, 3), put32(partitions, 24, rawGroups), put32(partitions, 28, 3);
    std::vector<std::uint8_t> raws(0x18, 0);
    put64(raws, 0, 0x80), put64(raws, 8, PartitionData - 0x80), put32(raws, 16, 0), put32(raws, 20, rawGroups);
    raws = squeeze(raws);
    std::vector<std::uint8_t> file(0x48 + 0xDC, 0);
    const std::size_t partitionsAt = file.size(), rawsAt = partitionsAt + partitions.size(), groupsAt = rawsAt + raws.size();
    std::vector<std::uint8_t> table(groups.size() * 12, 0);
    std::size_t at = (groupsAt + table.size() + 3) & ~std::size_t(3);
    std::vector<std::uint8_t> data;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (!groups[g].empty()) put32(table, 12 * g, std::uint32_t((at + data.size()) >> 2));
        put32(table, 12 * g + 4, std::uint32_t(groups[g].size()) | (groups[g].empty() ? 0 : flags[g][0])), put32(table, 12 * g + 8, flags[g][1]);
        data.insert(data.end(), groups[g].begin(), groups[g].end());
        data.resize((data.size() + 3) & ~std::size_t(3));
    }
    table = squeeze(table);
    std::memcpy(file.data(), "RVZ\x01", 4);
    put32(file, 4, 0x01000000), put32(file, 8, 0x00030000), put32(file, 0xC, 0xDC), put64(file, 0x24, disc.size());
    const std::size_t h = 0x48;
    put32(file, h, 2), put32(file, h + 4, 5), put32(file, h + 8, 5), put32(file, h + 0xC, 0x8000);
    std::copy(disc.begin(), disc.begin() + 0x80, file.begin() + h + 0x10);
    put32(file, h + 0x90, 1), put32(file, h + 0x94, 0x30), put64(file, h + 0x98, partitionsAt);
    put32(file, h + 0xB4, 1), put64(file, h + 0xB8, rawsAt), put32(file, h + 0xC0, std::uint32_t(raws.size()));
    put32(file, h + 0xC4, std::uint32_t(groups.size())), put64(file, h + 0xC8, groupsAt), put32(file, h + 0xD0, std::uint32_t(table.size()));
    file.insert(file.end(), partitions.begin(), partitions.end());
    file.insert(file.end(), raws.begin(), raws.end());
    file.insert(file.end(), table.begin(), table.end());
    file.resize(at);
    file.insert(file.end(), data.begin(), data.end());
    put64(file, 0x2C, file.size());
    return file;
}

void readBack(const std::vector<std::uint8_t>& image, const std::uint8_t common[16], const char* kind, smg::WiiDisc::Decompress decompress = {}) {
    smg::WiiDisc disc;
    disc.decompressWith(std::move(decompress));
    std::string error;
    check(disc.open(readerOf(image), common, nullptr, error), (std::string(kind) + " opens: " + error).c_str());
    check(disc.gameId() == "RMGE01" && disc.fileTable().size() == 0x40, "its game ID and file table");
    std::vector<std::uint8_t> file(FileSize);
    check(disc.read(FileAt, file.data(), FileSize), "a file across a cluster's edge reads");
    bool same = true;
    for (std::uint32_t i = 0; i < FileSize; ++i) same &= file[i] == std::uint8_t(i * 7 + 3);
    check(same, "and holds what was written");
}

void discs() {
    std::uint8_t common[16], title[16], wrong[16];
    for (int i = 0; i < 16; ++i) common[i] = std::uint8_t(0xA0 + i), title[i] = std::uint8_t(0x30 + 3 * i), wrong[i] = std::uint8_t(i);
    const std::vector<std::uint8_t> iso = discImage(common, title);
    readBack(iso, common, "an ISO");
    readBack(wbfsImage(iso), common, "a WBFS");
    std::uint8_t seed[68];
    for (int i = 0; i < 68; ++i) seed[i] = std::uint8_t(i * 37 + 11);
    const std::vector<std::uint8_t> rvz = rvzImage(iso, seed);
    readBack(rvz, nullptr, "an RVZ, with no key", unsqueeze);
    {
        smg::WiiDisc disc;
        disc.decompressWith(unsqueeze);
        std::string error;
        check(disc.open(readerOf(rvz), nullptr, nullptr, error) && !std::strcmp(disc.format(), "RVZ"), "the RVZ says it is one");
        std::vector<std::uint8_t> junk(JunkSize);
        check(disc.read(JunkFrom - 0x100, junk.data(), 0x100) && junk[0] == 0x00 && junk[0xFF] == 0xFF, "a packed group's literal bytes read");
        check(disc.read(JunkFrom, junk.data(), JunkSize) && junk == describedJunk(seed, JunkFrom % 0x8000, JunkSize),
              "its junk is what the seed gives, from its place in the 0x8000s");
        smg::WiiDisc without;
        check(!without.open(readerOf(rvz), nullptr, nullptr, error) && error.find("Zstandard") != std::string::npos, "an RVZ with no decompressor is told");
    }
    smg::WiiDisc disc;
    std::string error;
    check(!disc.open(readerOf(iso), wrong, nullptr, error) && error.find("did not decrypt") != std::string::npos, "a wrong common key is told");
    check(disc.open(readerOf(iso), nullptr, title, error), "a title key given directly opens it too");
}
} // namespace

int main() {
    fipsExample();
    discs();
    std::printf("%u Wii disc checks passed\n", checks);
    return 0;
}
