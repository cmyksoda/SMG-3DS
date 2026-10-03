// disc_tool IMAGE KEY [EXTRACTED]: opens a Wii disc image (ISO or WBFS) with the port's reader. KEY is the common key,
// or with a "title:" prefix the data partition's decrypted title key (as wit dump shows it), in hex. With EXTRACTED (a
// folder wit extracted from the same disc), every file in the image is read and compared with it.
#include <smg/WiiDisc.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return std::fprintf(stderr, "Usage: disc_tool IMAGE KEY|title:KEY [EXTRACTED]\n"), 2;
    FILE* image = std::fopen(argv[1], "rb");
    if (!image) return std::fprintf(stderr, "%s: cannot open\n", argv[1]), 1;
    const bool title = !std::strncmp(argv[2], "title:", 6);
    std::uint8_t key[16];
    if (!hex(argv[2] + (title ? 6 : 0), key)) return std::fprintf(stderr, "the key is not 32 hex digits\n"), 2;
    smg::WiiDisc disc;
    std::string error;
    const auto reader = [image](std::uint64_t offset, void* out, std::uint32_t size) {
        return fseeko(image, static_cast<off_t>(offset), SEEK_SET) == 0 && std::fread(out, 1, size, image) == size;
    };
    if (!disc.open(reader, title ? nullptr : key, title ? key : nullptr, error)) return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    const std::vector<std::uint8_t>& table = disc.fileTable();
    const std::uint32_t count = big32(&table[8]);
    std::printf("%s: %s, game %s, %u file table entries\n", argv[1], disc.format(), disc.gameId().c_str(), count);
    if (argc < 4) return 0;
    // Each entry's path from the directories holding it, then each file read from the image and from the folder.
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
