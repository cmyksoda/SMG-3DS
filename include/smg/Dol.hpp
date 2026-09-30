#pragma once
#include <smg/Archive.hpp>
#include <string>
#include <vector>

namespace smg {
// The BCSV tables the game links into main.dol rather than reading from an archive, copied out big-endian as the
// game's own code attaches them. Each is found by its fields, so any region's DOL serves.
struct DolTables {
    std::vector<std::uint8_t> galaxyId;    // GalaxyIDBCSV: name, PowerStarNum, GrandGalaxyNo, ...
    std::vector<std::uint8_t> storyEvent;  // StoryEventBCSV: name, progress
};
bool findDolTables(Bytes dol, DolTables& tables, std::string& error);
} // namespace smg
