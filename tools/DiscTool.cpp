// disc_tool IMAGE KEY|title:KEY|- [EXTRACTED]: reads every file of an ISO, WBFS or RVZ against wit's extraction.
// disc_tool --rvz EXTRACTED OUT: an RVZ rebuilt from wit's folder for trying the port's RVZ path; not a real dump.
#include <smg/WiiDisc.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if __has_include(<zstd.h>)
#include <zstd.h>
#define DISC_TOOL_ZSTD 1
#endif

namespace {
bool hex(const char* text, std::uint8_t out[16]) {
    std::string digits;
    for (const char* c = text; *c; ++c)
        if (*c != ' ' && *c != ':') digits += *c;
    if (digits.size() != 32) return false;
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(std::stoul(digits.substr(2 * i, 2), nullptr, 16));
    return true;
}
std::uint32_t big32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }

#ifdef DISC_TOOL_ZSTD
void put32(std::uint8_t* p, std::uint32_t v) { p[0] = std::uint8_t(v >> 24), p[1] = std::uint8_t(v >> 16), p[2] = std::uint8_t(v >> 8), p[3] = std::uint8_t(v); }
void put64(std::uint8_t* p, std::uint64_t v) { put32(p, std::uint32_t(v >> 32)), put32(p + 4, std::uint32_t(v)); }

std::vector<std::uint8_t> whole(const std::string& path) {
    std::vector<std::uint8_t> bytes;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        std::fseek(file, 0, SEEK_END);
        bytes.resize(std::size_t(std::ftell(file)));
        std::fseek(file, 0, SEEK_SET);
        if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) bytes.clear();
        std::fclose(file);
    }
    return bytes;
}

std::vector<std::uint8_t> squeeze(const std::vector<std::uint8_t>& in) {
    std::vector<std::uint8_t> out(ZSTD_compressBound(in.size()));
    out.resize(ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3));
    return out;
}

int writeRvz(const std::string& folder, const char* outPath) {
    struct Piece {
        std::uint64_t at;
        std::uint32_t size;
        std::string path;
    };
    const std::vector<std::uint8_t> boot = whole(folder + "/sys/boot.bin"), table = whole(folder + "/sys/fst.bin");
    if (boot.size() < 0x440 || table.size() < 12) return std::fprintf(stderr, "%s/sys is not wit's\n", folder.c_str()), 1;
    std::vector<Piece> pieces;
    const auto add = [&](std::uint64_t at, const std::string& path) {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        pieces.push_back({at, std::uint32_t(std::ftell(file)), path});
        std::fclose(file);
        return true;
    };
    if (!add(0, folder + "/sys/boot.bin") || !add(0x440, folder + "/sys/bi2.bin") || !add(0x2440, folder + "/sys/apploader.img") ||
        !add(std::uint64_t(big32(&boot[0x420])) << 2, folder + "/sys/main.dol") || !add(std::uint64_t(big32(&boot[0x424])) << 2, folder + "/sys/fst.bin"))
        return std::fprintf(stderr, "%s/sys is missing a file\n", folder.c_str()), 1;
    const std::uint32_t count = big32(&table[8]);
    std::vector<std::uint32_t> ends(count, count), stack{0};
    std::vector<std::string> paths(count);
    const auto* names = reinterpret_cast<const char*>(&table[12 * count]);
    for (std::uint32_t i = 1; i < count; ++i) {
        const std::uint8_t* raw = &table[12 * i];
        if (raw[0]) ends[i] = big32(raw + 8);
    }
    for (std::uint32_t i = 1; i < count; ++i) {
        while (i >= ends[stack.back()]) stack.pop_back();
        const std::uint8_t* raw = &table[12 * i];
        paths[i] = paths[stack.back()] + "/" + (names + (big32(raw) & 0xFFFFFF));
        if (raw[0]) stack.push_back(i);
        else if (big32(raw + 8)) pieces.push_back({std::uint64_t(big32(raw + 4)) << 2, big32(raw + 8), folder + "/files" + paths[i]});
    }
    std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.at < b.at; });
    std::uint64_t end = 0;
    for (const Piece& p : pieces) end = std::max(end, p.at + p.size);
    const std::uint64_t clusters = (end + 0x7BFF) / 0x7C00, dataSize = clusters * 0x7C00;

    // The ticket's key and signatures stay empty: an RVZ keeps the partition decrypted.
    constexpr std::uint64_t Partition = 0x50000, DataAt = Partition + 0x20000;
    constexpr std::uint32_t Chunk = 0x20000, DataChunk = Chunk / 0x8000 * 0x7C00;
    std::vector<std::uint8_t> disc(DataAt, 0);
    std::copy(boot.begin(), boot.end(), disc.begin());
    put32(&disc[0x40000], 1), put32(&disc[0x40004], 0x40020 >> 2), put32(&disc[0x40020], Partition >> 2);
    put32(&disc[Partition + 0x2B8], 0x20000 >> 2), put32(&disc[Partition + 0x2BC], std::uint32_t(clusters * 0x8000 >> 2));

    FILE* out = std::fopen(outPath, "wb");
    if (!out) return std::fprintf(stderr, "%s: cannot write\n", outPath), 1;
    std::uint64_t at = 0x48 + 0xDC + 0x30;
    std::vector<std::uint8_t> groups;  // the group table, 12 bytes a group
    std::fseek(out, long(at), SEEK_SET);
    const auto group = [&](const std::vector<std::uint8_t>& payload, bool empty) {
        std::uint8_t entry[12] = {};
        if (!empty) {
            const std::vector<std::uint8_t> stored = squeeze(payload);
            put32(entry, std::uint32_t(at >> 2)), put32(entry + 4, 0x80000000u | std::uint32_t(stored.size()));
            std::fwrite(stored.data(), 1, stored.size(), out);
            at += stored.size();
            for (; at % 4; ++at) std::fputc(0, out);
        }
        groups.insert(groups.end(), entry, entry + 12);
    };
    const std::uint32_t rawGroups = (DataAt + Chunk - 1) / Chunk;
    for (std::uint32_t g = 0; g < rawGroups; ++g) {
        const std::uint64_t from = std::uint64_t(g) * Chunk, size = std::min<std::uint64_t>(Chunk, DataAt - from);
        std::vector<std::uint8_t> payload(disc.begin() + std::ptrdiff_t(from), disc.begin() + std::ptrdiff_t(from + size));
        group(payload, std::all_of(payload.begin(), payload.end(), [](std::uint8_t b) { return b == 0; }));
    }
    const std::uint32_t dataGroups = std::uint32_t((dataSize + DataChunk - 1) / DataChunk);
    std::size_t next = 0;
    for (std::uint32_t g = 0; g < dataGroups; ++g) {
        const std::uint64_t from = std::uint64_t(g) * DataChunk, size = std::min<std::uint64_t>(DataChunk, dataSize - from);
        std::vector<std::uint8_t> payload(2 + size, 0);  // an empty hash exception list, then the data
        bool empty = true;
        while (next < pieces.size() && pieces[next].at + pieces[next].size <= from) ++next;
        for (std::size_t p = next; p < pieces.size() && pieces[p].at < from + size; ++p) {
            const std::uint64_t lo = std::max(from, pieces[p].at), hi = std::min(from + size, pieces[p].at + pieces[p].size);
            if (lo >= hi) continue;
            FILE* file = std::fopen(pieces[p].path.c_str(), "rb");
            if (!file || fseeko(file, off_t(lo - pieces[p].at), SEEK_SET) || std::fread(&payload[2 + (lo - from)], 1, hi - lo, file) != hi - lo)
                return std::fprintf(stderr, "%s: cannot read\n", pieces[p].path.c_str()), 1;
            std::fclose(file);
            empty = false;
        }
        group(payload, empty);
        if (g % 2048 == 0) std::fprintf(stderr, "\r%u of %u groups", g, dataGroups);
    }
    std::fprintf(stderr, "\n");

    std::vector<std::uint8_t> raws(0x18, 0);
    put64(&raws[0], 0x80), put64(&raws[8], DataAt - 0x80), put32(&raws[16], 0), put32(&raws[20], rawGroups);
    const std::vector<std::uint8_t> rawTable = squeeze(raws), groupTable = squeeze(groups);
    const std::uint64_t rawAt = at, groupAt = at + rawTable.size();
    std::fwrite(rawTable.data(), 1, rawTable.size(), out), std::fwrite(groupTable.data(), 1, groupTable.size(), out);
    const std::uint64_t fileSize = groupAt + groupTable.size();

    std::uint8_t head[0x48 + 0xDC + 0x30] = {};
    std::memcpy(head, "RVZ\x01", 4);
    put32(head + 4, 0x01000000), put32(head + 8, 0x00030000), put32(head + 0xC, 0xDC);
    put64(head + 0x24, DataAt + clusters * 0x8000), put64(head + 0x2C, fileSize);
    std::uint8_t* h = head + 0x48;
    put32(h, 2), put32(h + 4, 5), put32(h + 8, 3), put32(h + 0xC, Chunk);
    std::copy(boot.begin(), boot.begin() + 0x80, h + 0x10);
    put32(h + 0x90, 1), put32(h + 0x94, 0x30), put64(h + 0x98, 0x48 + 0xDC);
    put32(h + 0xB4, 1), put64(h + 0xB8, rawAt), put32(h + 0xC0, std::uint32_t(rawTable.size()));
    put32(h + 0xC4, std::uint32_t(groups.size() / 12)), put64(h + 0xC8, groupAt), put32(h + 0xD0, std::uint32_t(groupTable.size()));
    std::uint8_t* partition = head + 0x48 + 0xDC;
    put32(partition + 16, std::uint32_t(DataAt / 0x8000)), put32(partition + 20, std::uint32_t(clusters));
    put32(partition + 24, rawGroups), put32(partition + 28, dataGroups);
    std::fseek(out, 0, SEEK_SET);
    std::fwrite(head, 1, sizeof head, out);
    std::fclose(out);
    std::printf("%s: %zu files, %.1f MB of data in %u groups, %.1f MB\n", outPath, pieces.size(), dataSize / 1048576.0, dataGroups, fileSize / 1048576.0);
    return 0;
}
#endif
} // namespace

int main(int argc, char** argv) {
#ifdef DISC_TOOL_ZSTD
    if (argc == 4 && !std::strcmp(argv[1], "--rvz")) return writeRvz(argv[2], argv[3]);
#endif
    if (argc < 3) return std::fprintf(stderr, "Usage: disc_tool IMAGE KEY|title:KEY|- [EXTRACTED]\n       disc_tool --rvz EXTRACTED OUT\n"), 2;
    FILE* image = std::fopen(argv[1], "rb");
    if (!image) return std::fprintf(stderr, "%s: cannot open\n", argv[1]), 1;
    const bool none = !std::strcmp(argv[2], "-"), title = !std::strncmp(argv[2], "title:", 6);
    std::uint8_t key[16] = {};
    if (!none && !hex(argv[2] + (title ? 6 : 0), key)) return std::fprintf(stderr, "the key is not 32 hex digits\n"), 2;
    smg::WiiDisc disc;
#ifdef DISC_TOOL_ZSTD
    disc.decompressWith([](const void* in, std::size_t size, void* out, std::size_t capacity) {
        const std::size_t made = ZSTD_decompress(out, capacity, in, size);
        return ZSTD_isError(made) ? SIZE_MAX : made;
    });
#endif
    std::string error;
    const auto reader = [image](std::uint64_t offset, void* out, std::uint32_t size) {
        return fseeko(image, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(out, 1, size, image) == size;
    };
    if (!disc.open(reader, none || title ? nullptr : key, title ? key : nullptr, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    const std::vector<std::uint8_t>& table = disc.fileTable();
    const std::uint32_t count = big32(&table[8]);
    std::printf("%s: %s, game %s, %u file table entries\n", argv[1], disc.format(), disc.gameId().c_str(), count);
    if (argc < 4) return 0;
    std::vector<std::string> paths(count);
    std::vector<std::uint32_t> ends(count, count);
    const auto* names = reinterpret_cast<const char*>(&table[12 * count]);
    for (std::uint32_t i = 1; i < count; ++i) {
        const std::uint8_t* raw = &table[12 * i];
        if (raw[0]) ends[i] = big32(raw + 8);
    }
    std::vector<std::uint32_t> stack{0};
    unsigned files = 0, differing = 0;
    std::uint64_t bytes = 0;
    for (std::uint32_t i = 1; i < count; ++i) {
        while (i >= ends[stack.back()]) stack.pop_back();
        const std::uint8_t* raw = &table[12 * i];
        paths[i] = paths[stack.back()] + "/" + (names + (big32(raw) & 0xFFFFFF));
        if (raw[0]) {
            stack.push_back(i);
            continue;
        }
        const std::uint64_t offset = std::uint64_t(big32(raw + 4)) << 2;
        const std::uint32_t size = big32(raw + 8);
        std::vector<std::uint8_t> fromImage(size), fromFolder(size);
        const bool read = disc.read(offset, fromImage.data(), size);
        FILE* file = std::fopen((std::string(argv[3]) + paths[i]).c_str(), "rb");
        const bool other = file && std::fread(fromFolder.data(), 1, size, file) == size;
        if (file) std::fclose(file);
        ++files, bytes += size;
        if (!read || !other || fromImage != fromFolder) {
            if (++differing <= 10) std::printf("differs: %s (%s)\n", paths[i].c_str(), !read ? "image unread" : !other ? "folder unread" : "bytes");
        }
    }
    std::printf("%u files, %.1f MB compared: %u differ\n", files, bytes / 1048576.0, differing);
    std::fclose(image);
    return differing ? 1 : 0;
}
