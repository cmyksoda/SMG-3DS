#include <smg/Dol.hpp>
#include <JSystem/JGadget/hashcode.hpp>
#include <cstring>

namespace smg {
namespace {
std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]; }
std::uint32_t be16(const std::uint8_t* p) { return std::uint32_t(p[0]) << 8 | p[1]; }

// A big-endian table starting at `at`, if one plausibly does: its length (records and the strings they name), or 0.
std::size_t tableAt(const std::uint8_t* at, std::size_t available) {
    if (available < 16) return 0;
    const std::uint32_t count = be32(at), fields = be32(at + 4), offset = be32(at + 8), stride = be32(at + 12);
    if (!fields || fields > 64 || !count || count > 4096 || offset != 16 + fields * 12 || !stride || stride > 1024) return 0;
    if (offset + std::size_t(count) * stride > available) return 0;
    std::size_t end = offset + std::size_t(count) * stride;
    const std::size_t strings = end;
    for (std::uint32_t i = 0; i < fields; ++i) {
        const std::uint8_t* item = at + 16 + i * 12;
        if (item[11] > 6 || be16(item + 8) >= stride) return 0;
        if (item[11] != 6) continue;
        for (std::uint32_t row = 0; row < count; ++row) {
            const std::size_t name = strings + be32(at + offset + row * stride + be16(item + 8));
            const void* terminator = name < available ? std::memchr(at + name, 0, available - name) : nullptr;
            if (!terminator) return 0;
            end = std::max(end, std::size_t(static_cast<const std::uint8_t*>(terminator) - at) + 1);
        }
    }
    return end;
}

bool hasFields(const std::uint8_t* table, std::initializer_list<const char*> names) {
    const std::uint32_t fields = be32(table + 4);
    for (const char* name : names) {
        const std::uint32_t hash = JGadget::getHashCode(name);
        bool found = false;
        for (std::uint32_t i = 0; i < fields && !found; ++i) found = be32(table + 16 + i * 12) == hash;
        if (!found) return false;
    }
    return true;
}
} // namespace

bool findDolTables(Bytes dol, DolTables& tables, std::string& error) {
    tables = {};
    if (!dol.data || dol.size < 0x100) { error = "main.dol is too short for its header"; return false; }
    // Data sections 7 to 17: file offsets, then load addresses, then sizes, 18 words each.
    for (int section = 7; section < 18; ++section) {
        const std::size_t offset = be32(dol.data + section * 4), size = be32(dol.data + 0x90 + section * 4);
        if (!size || offset > dol.size || size > dol.size - offset) continue;
        const std::uint8_t* data = dol.data + offset;
        for (std::size_t at = 0; at + 16 <= size; at += 4) {
            const std::size_t length = tableAt(data + at, size - at);
            if (!length) continue;
            if (tables.galaxyId.empty() && hasFields(data + at, {"name", "PowerStarNum", "GrandGalaxyNo"}))
                tables.galaxyId.assign(data + at, data + at + length);
            else if (tables.storyEvent.empty() && be32(data + at + 4) == 2 && hasFields(data + at, {"name", "progress"}))
                tables.storyEvent.assign(data + at, data + at + length);
        }
    }
    if (tables.galaxyId.empty() || tables.storyEvent.empty()) {
        error = tables.galaxyId.empty() ? "main.dol holds no GalaxyIDBCSV" : "main.dol holds no StoryEventBCSV";
        return false;
    }
    return true;
}
} // namespace smg
