#include <smg/Archive.hpp>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 2 && argc != 5) {
        std::fprintf(stderr, "Usage: archive_tool ARCHIVE [--extract ENTRY OUTPUT]\n");
        return 2;
    }
    std::vector<std::uint8_t> storage;
    std::vector<smg::ArchiveEntry> entries;
    std::string error;
    // Offline tool budget; the 3DS runtime must supply its current free-memory budget.
    if (!smg::readArchive(argv[1], storage, entries, 512u*1024*1024, error)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str()); return 1;
    }
    if (argc == 2) {
        for (const auto& entry : entries) std::printf("%u\t%s\n", entry.size, entry.path.c_str());
        return 0;
    }
    if (std::strcmp(argv[2], "--extract")) return 2;
    for (const auto& entry : entries) if (entry.path == argv[3]) {
        FILE* output = std::fopen(argv[4], "wbx");
        if (!output) { std::perror(argv[4]); return 1; }
        const auto written = std::fwrite(storage.data()+entry.offset, 1, entry.size, output);
        const auto closed = std::fclose(output);
        return written == entry.size && !closed ? 0 : 1;
    }
    std::fprintf(stderr, "Archive entry not found: %s\n", argv[3]); return 1;
}
