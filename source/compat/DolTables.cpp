// The BCSV tables the game links into main.dol (GalaxyIDBCSV, StoryEventBCSV), read from the disc's main.dol (in the
// disc image, or sys/main.dol) on first use and kept for the program's life, big-endian as the game's code attaches them.
#include <smg/DiscImage.hpp>
#include <smg/Dol.hpp>
#include <smg/DolTables.hpp>
#include <smg/FreeStore.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace smg {
namespace {
constexpr const char* sDolPath = "sdmc:/3ds/SMG-3DS/sys/main.dol";

std::uint32_t big32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }

[[noreturn]] void stop(const char* what, const char* detail) {
    std::fprintf(stderr, "main.dol's tables: %s%s\n", what, detail);
    std::abort();
}

const DolTables& tables() {
    static DolTables found;
    static bool read = false;
    if (read) return found;
    std::vector<std::uint8_t> dol;
    if (disc::fromImage()) {
        // Where boot.bin says, as long as its furthest section reaches (18 offsets, then their addresses, then sizes).
        const std::uint8_t* boot = disc::bootBin();
        const std::uint64_t at = std::uint64_t(big32(boot + 0x420)) << 2;
        std::uint8_t head[0x100];
        if (!disc::read(at, head, sizeof head)) stop("main.dol's header could not be read from the disc image", "");
        std::uint32_t size = sizeof head;
        for (int section = 0; section < 18; ++section)
            if (big32(head + 0x90 + 4 * section)) size = std::max(size, big32(head + 4 * section) + big32(head + 0x90 + 4 * section));
        dol.resize(size);
        if (!disc::read(at, dol.data(), size)) stop("main.dol could not be read from the disc image", "");
    } else {
        FILE* file = std::fopen(sDolPath, "rb");
        if (!file) stop("sys/main.dol is not on the SD card (3ds/SMG-3DS/sys)", "");
        std::fseek(file, 0, SEEK_END);
        dol.resize(std::ftell(file));
        std::fseek(file, 0, SEEK_SET);
        const bool whole = std::fread(dol.data(), 1, dol.size(), file) == dol.size();
        std::fclose(file);
        if (!whole) stop("sys/main.dol could not be read", "");
    }
    std::string error;
    if (!findDolTables({dol.data(), dol.size()}, found, error)) stop(error.c_str(), "");
    read = true;
    return found;
}
} // namespace

const void* dolTable(const char* name) {
    // Kept for the program's life, as the DOL's own data was; the game turns each native as it attaches it. Reading
    // them takes the whole DOL for a moment, never from the game's heaps.
    const PortMemory memory;
    auto& all = const_cast<DolTables&>(tables());
    if (!std::strcmp(name, "GalaxyIDBCSV")) return all.galaxyId.data();
    if (!std::strcmp(name, "StoryEventBCSV")) return all.storyEvent.data();
    stop("no table named ", name);
}
} // namespace smg
