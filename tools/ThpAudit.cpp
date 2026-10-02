// Decodes every video frame of the disc's THP movies through smg::decodeThpVideo, and writes every Nth frame twice for
// tools/thp-audit.py: as a standard JPEG (its scan stuffed again) and as the luma plane the decoder made.
// usage: thp_audit MOVIE_DIR OUT_DIR EVERY
#include <smg/Thp.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

namespace {
std::uint32_t big32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }

// A standard decoder wants a zero after each 0xFF in the scan; THP leaves it out.
std::vector<std::uint8_t> stuffed(const std::uint8_t* frame, std::size_t size) {
    std::size_t at = 2;
    while (at + 4 <= size && frame[at + 1] != 0xDA) at += 2 + (frame[at + 2] << 8 | frame[at + 3]);
    const std::size_t scan = at + 2 + (frame[at + 2] << 8 | frame[at + 3]);
    std::size_t end = size;
    while (end > scan + 1 && !(frame[end - 2] == 0xFF && frame[end - 1] == 0xD9)) --end;
    std::vector<std::uint8_t> out(frame, frame + scan);
    for (std::size_t i = scan; i + 2 < end; ++i) {
        out.push_back(frame[i]);
        if (frame[i] == 0xFF) out.push_back(0);
    }
    out.push_back(0xFF), out.push_back(0xD9);
    return out;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) return std::fputs("usage: thp_audit MOVIE_DIR OUT_DIR EVERY\n", stderr), 2;
    const unsigned every = static_cast<unsigned>(std::atoi(argv[3]));
    DIR* dir = opendir(argv[1]);
    if (!dir) return std::perror(argv[1]), 2;
    unsigned movies = 0, frames = 0, failed = 0, written = 0;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".thp")) continue;
        FILE* f = std::fopen((std::string(argv[1]) + "/" + name).c_str(), "rb");
        std::vector<std::uint8_t> head(0x80);
        if (!f || std::fread(head.data(), 1, head.size(), f) != head.size() || std::memcmp(head.data(), "THP", 4)) {
            std::fprintf(stderr, "%s: not a THP movie\n", name.c_str());
            ++failed;
            continue;
        }
        const std::uint32_t numFrames = big32(&head[0x14]), firstSize = big32(&head[0x18]), compInfo = big32(&head[0x20]), data = big32(&head[0x28]);
        const std::uint32_t components = big32(&head[compInfo]);
        const unsigned width = big32(&head[compInfo + 0x14]), height = big32(&head[compInfo + 0x18]);
        std::vector<std::uint8_t> y(std::size_t(width) * height), u(y.size() / 4), v(y.size() / 4), frame;
        std::uint32_t offset = data, size = firstSize;
        for (std::uint32_t i = 0; i < numFrames; ++i) {
            frame.resize(size);
            if (std::fseek(f, offset, SEEK_SET) || std::fread(frame.data(), 1, size, f) != size) {
                std::fprintf(stderr, "%s frame %u: cut off\n", name.c_str(), i);
                ++failed;
                break;
            }
            const std::uint32_t videoSize = big32(&frame[8]);
            const std::uint8_t* video = &frame[8 + 4 * components];
            if (const char* why = smg::decodeThpVideo({video, videoSize}, width, height, y.data(), u.data(), v.data())) {
                std::fprintf(stderr, "%s frame %u: %s\n", name.c_str(), i, why);
                ++failed;
            } else if (every && i % every == 0) {
                const std::string base = std::string(argv[2]) + "/" + name.substr(0, name.size() - 4) + "-" + std::to_string(i);
                const auto jpeg = stuffed(video, videoSize);
                FILE* out = std::fopen((base + ".jpg").c_str(), "wb");
                FILE* luma = std::fopen((base + ".y").c_str(), "wb");
                if (out && luma) std::fwrite(jpeg.data(), 1, jpeg.size(), out), std::fwrite(y.data(), 1, y.size(), luma), ++written;
                if (out) std::fclose(out);
                if (luma) std::fclose(luma);
            }
            ++frames;
            offset += size;
            size = big32(&frame[0]);
        }
        std::fclose(f);
        ++movies;
        std::printf("%s: %ux%u, %u frames\n", name.c_str(), width, height, numFrames);
    }
    closedir(dir);
    std::printf("%u movies, %u frames decoded, %u failed, %u written for comparison\n", movies, frames, failed, written);
    return failed ? 1 : 0;
}
