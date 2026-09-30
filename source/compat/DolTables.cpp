// The BCSV tables the game links into main.dol (GalaxyIDBCSV, StoryEventBCSV), read from the disc's sys/main.dol on
// first use and kept for the program's life, big-endian as the game's code attaches them.
#include <smg/Dol.hpp>
#include <smg/DolTables.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace smg {
namespace {
constexpr const char* sDolPath = "sdmc:/3ds/SMG-3DS/sys/main.dol";

[[noreturn]] void stop(const char* what, const char* detail) {
    std::fprintf(stderr, "main.dol's tables: %s%s\n", what, detail);
    std::abort();
}

const DolTables& tables() {
    static DolTables found;
    static bool read = false;
    if (read) return found;
    FILE* file = std::fopen(sDolPath, "rb");
    if (!file) stop("sys/main.dol is not on the SD card (3ds/SMG-3DS/sys)", "");
    std::fseek(file, 0, SEEK_END);
    std::vector<std::uint8_t> dol(std::ftell(file));
    std::fseek(file, 0, SEEK_SET);
    const bool whole = std::fread(dol.data(), 1, dol.size(), file) == dol.size();
    std::fclose(file);
    if (!whole) stop("sys/main.dol could not be read", "");
    std::string error;
    if (!findDolTables({dol.data(), dol.size()}, found, error)) stop(error.c_str(), "");
    read = true;
    return found;
}
} // namespace

const void* dolTable(const char* name) {
    // Kept for the program's life, as the DOL's own data was; the game turns each native as it attaches it.
    auto& all = const_cast<DolTables&>(tables());
    if (!std::strcmp(name, "GalaxyIDBCSV")) return all.galaxyId.data();
    if (!std::strcmp(name, "StoryEventBCSV")) return all.storyEvent.data();
    stop("no table named ", name);
}
} // namespace smg
