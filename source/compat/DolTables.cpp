// The BCSV tables main.dol carries in its data (GalaxyIDBCSV, StoryEventBCSV). The disc's sys/main.dol holds
// them; until the port reads them from there, asking for one stops the program by name.
#include <smg/DolTables.hpp>
#include <cstdio>
#include <cstdlib>

namespace smg {
const void* dolTable(const char* name) {
    std::fprintf(stderr, "main.dol's %s is not read by the port yet\n", name);
    std::abort();
}
} // namespace smg
