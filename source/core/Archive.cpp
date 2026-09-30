#include <smg/Archive.hpp>
#include <dirent.h>
#include <strings.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

namespace smg {
namespace {
std::uint16_t be16(const std::uint8_t* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
bool fail(std::string& error, const char* reason) { error = reason; return false; }
bool table(Bytes data, std::size_t offset, std::size_t count, std::size_t stride) {
    return offset <= data.size && count <= (data.size - offset) / stride;
}
}

bool decodeYaz0(Bytes source, std::vector<std::uint8_t>& output, std::size_t memoryBudget, std::string& error) {
    output.clear(); error.clear();
    if (!source.data || source.size < 16 || std::memcmp(source.data, "Yaz0", 4)) return fail(error, "Invalid Yaz0 header");
    const auto size = be32(source.data + 4);
    if (size > memoryBudget) return fail(error, "Yaz0 output exceeds memory budget");
    std::vector<std::uint8_t> decoded(size);
    std::size_t src = 16, dst = 0;
    while (dst < size) {
        if (!source.contains(src, 1)) return fail(error, "Truncated Yaz0 control byte");
        const auto control = source.data[src++];
        for (unsigned mask = 0x80; mask && dst < size; mask >>= 1) {
            if (control & mask) {
                if (!source.contains(src, 1)) return fail(error, "Truncated Yaz0 literal");
                decoded[dst++] = source.data[src++];
            } else {
                if (!source.contains(src, 2)) return fail(error, "Truncated Yaz0 back-reference");
                const auto first = source.data[src++];
                const std::size_t distance = ((first & 15) << 8) + source.data[src++] + 1;
                std::size_t count = first >> 4;
                if (!count) {
                    if (!source.contains(src, 1)) return fail(error, "Truncated Yaz0 long run");
                    count = source.data[src++] + 0x12;
                } else count += 2;
                if (distance > dst || count > size - dst) return fail(error, "Invalid Yaz0 back-reference bounds");
                for (std::size_t i = 0; i < count; ++i, ++dst) decoded[dst] = decoded[dst - distance];
            }
        }
    }
    output.swap(decoded);
    return true;
}

bool indexRarc(Bytes source, std::vector<ArchiveEntry>& entries, std::string& error) {
    entries.clear(); error.clear();
    if (!source.data || source.size < 64 || std::memcmp(source.data, "RARC", 4)) return fail(error, "Invalid RARC header");
    const auto fileSize = be32(source.data + 4);
    const std::size_t base = be32(source.data + 8);
    if (fileSize > source.size || fileSize < 64 || base != 32) return fail(error, "Invalid RARC file size or header size");
    source.size = fileSize;
    const auto relative = [&](std::size_t at, std::size_t& result) {
        const std::size_t offset = be32(source.data + at);
        if (offset > source.size - base) return false;
        result = base + offset;
        return true;
    };
    std::size_t dataOffset, nodes, files, strings;
    if (!relative(12, dataOffset) || !relative(36, nodes) || !relative(44, files) || !relative(52, strings))
        return fail(error, "RARC table offset outside file");
    const auto dataSize = be32(source.data + 16);
    const auto nodeCount = be32(source.data + 32);
    const auto fileCount = be32(source.data + 40);
    const auto stringSize = be32(source.data + 48);
    if (!nodeCount || !table(source, nodes, nodeCount, 16) || !table(source, files, fileCount, 20) ||
        !source.contains(strings, stringSize) || !source.contains(dataOffset, dataSize))
        return fail(error, "RARC table exceeds file bounds");
    struct Pending { std::uint32_t node; std::string path; };
    std::vector<Pending> pending{{0, ""}};
    std::vector<bool> visited(nodeCount, false);
    std::set<std::string> paths;
    std::vector<ArchiveEntry> result;
    while (!pending.empty()) {
        auto current = std::move(pending.back()); pending.pop_back();
        if (current.node >= nodeCount || visited[current.node]) return fail(error, "Invalid or cyclic RARC directory");
        visited[current.node] = true;
        const auto* node = source.data + nodes + current.node * 16;
        const auto count = be16(node + 10);
        const auto first = be32(node + 12);
        if (first > fileCount || count > fileCount - first) return fail(error, "Invalid RARC directory entry range");
        for (std::size_t i = 0; i < count; ++i) {
            const auto* record = source.data + files + (first + i) * 20;
            const auto flags = record[4];
            const auto nameOffset = be32(record + 4) & 0xffffff;
            if (nameOffset >= stringSize) return fail(error, "Invalid RARC name offset");
            const char* name = reinterpret_cast<const char*>(source.data + strings + nameOffset);
            const auto* end = static_cast<const char*>(std::memchr(name, 0, stringSize - nameOffset));
            if (!end) return fail(error, "Unterminated RARC filename");
            const std::string component(name, end);
            if ((flags & 2) && (component == "." || component == "..")) continue;
            if (component.empty() || component == "." || component == ".." ||
                component.find_first_of("/\\:") != std::string::npos) return fail(error, "Unsafe RARC path component");
            const std::string path = current.path + component;
            if (!paths.insert(path).second) return fail(error, "Duplicate RARC path");
            const auto offset = be32(record + 8);
            if (flags & 2) {
                if (flags & 1) return fail(error, "RARC entry is both file and directory");
                pending.push_back({offset, path + "/"});
            } else if (flags & 1) {
                const auto size = be32(record + 12);
                if (offset > dataSize || size > dataSize - offset) return fail(error, "RARC file outside data region");
                result.push_back({path, static_cast<std::uint32_t>(dataOffset + offset), size, flags});
            } else return fail(error, "Unsupported RARC entry flags");
        }
    }
    entries.swap(result);
    return true;
}

// The Wii's DVD file system ignores case, and the disc relies on it (FloaterOtakingGalaxy.arc is
// asked for as FloaterOtaKingGalaxy); a case-sensitive host or SD folder needs each part matched.
namespace {
FILE* openDiscFile(const char* path) {
    if (FILE* file = std::fopen(path, "rb")) return file;
    std::string resolved, rest = path;
    if (const auto root = rest.find('/'); root != std::string::npos) {
        resolved = rest.substr(0, root + 1);
        rest.erase(0, root + 1);
    }
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const std::string part = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        DIR* directory = opendir(resolved.empty() ? "." : resolved.c_str());
        if (!directory) return nullptr;
        std::string match;
        while (const dirent* entry = readdir(directory)) {
            if (!strcasecmp(entry->d_name, part.c_str())) { match = entry->d_name; break; }
        }
        closedir(directory);
        if (match.empty()) return nullptr;
        resolved += match + (rest.empty() ? "" : "/");
    }
    return std::fopen(resolved.c_str(), "rb");
}
} // namespace

bool readArchive(const char* path, std::vector<std::uint8_t>& storage,
    std::vector<ArchiveEntry>& entries, std::size_t memoryBudget, std::string& error) {
    std::vector<std::uint8_t>().swap(storage);
    entries.clear(); error.clear();
    FILE* file = openDiscFile(path);
    if (!file) return fail(error, "Cannot open archive");
    if (std::fseek(file, 0, SEEK_END)) { std::fclose(file); return fail(error, "Cannot seek archive"); }
    const long length = std::ftell(file);
    if (length < 0 || static_cast<unsigned long>(length) > memoryBudget || std::fseek(file, 0, SEEK_SET)) {
        std::fclose(file); return fail(error, "Archive exceeds memory budget or cannot be read");
    }
    std::vector<std::uint8_t> compressed(static_cast<std::size_t>(length));
    const auto read = std::fread(compressed.data(), 1, compressed.size(), file);
    std::fclose(file);
    if (read != compressed.size()) return fail(error, "Truncated archive read");
    if (compressed.size() >= 4 && !std::memcmp(compressed.data(), "Yaz0", 4)) {
        if (!decodeYaz0({compressed.data(), compressed.size()}, storage, memoryBudget - compressed.size(), error)) return false;
        std::vector<std::uint8_t>().swap(compressed);
    } else storage.swap(compressed);
    if (!indexRarc({storage.data(), storage.size()}, entries, error)) {
        std::vector<std::uint8_t>().swap(storage);
        return false;
    }
    return true;
}
} // namespace smg
