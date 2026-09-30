#pragma once
// BCSV tables the game links into main.dol instead of reading from an archive (GalaxyIDBCSV, StoryEventBCSV).
namespace smg {
const void* dolTable(const char* name);
}
