// BCSV tables turned native in place as JMapInfo attaches them (compat/smg/NativeResources.hpp). A header reads
// as a plausible table in only one byte order, which says whether the table still needs turning.
#include <Game/Util/JMapInfo.hpp>
#include <smg/NativeResources.hpp>
#include <cstdio>
#include <cstdlib>

namespace {
bool plausible(u32 count, u32 fields, u32 offset, u32 stride) {
    return fields < 0x1000 && offset >= 16 + fields * 12 && offset < 0x1000000 && stride < 0x10000 && count < 0x100000 &&
           (count == 0 || stride > 0);
}

// A field's width by type, as JMapInfo reads it: an inline string of 32 bytes, a short, a byte, else a word.
u32 width(u8 type) { return type == 1 ? 32 : type == 4 ? 2 : type == 5 ? 1 : 4; }

[[noreturn]] void stop(const char* what) {
    std::fprintf(stderr, "A BCSV table %s\n", what);
    std::abort();
}
} // namespace

namespace {
struct Span {
    u32 at, size;
};

// Where each of a big-endian table's numeric values sits in a record, once each; the reason if it cannot be
// turned in place.
const char* spansOf(const u32* header, Span* spans, u32& spanCount) {
    const u32 fields = __builtin_bswap32(header[1]), stride = __builtin_bswap32(header[3]);
    const auto* items = reinterpret_cast<const JMapItem*>(header + 4);
    spanCount = 0;
    for (u32 i = 0; i < fields; ++i) {
        const u32 at = __builtin_bswap16(items[i].mOffsData), size = width(items[i].mType);
        if (size == 32 || size == 1) continue;
        // ARM loads a word or half from an address its size divides; a table that breaks that needs repacking.
        if (at % size || stride % 4) return "has fields that need repacking for ARM (smg::Bcsv repacks; the game's loading does not yet)";
        bool known = false;
        for (u32 s = 0; s < spanCount; ++s) {
            if (spans[s].at == at && spans[s].size == size) known = true;
            else if (at < spans[s].at + spans[s].size && spans[s].at < at + size) return "has overlapping fields of different widths";
        }
        if (!known) {
            if (spanCount == 64) return "has more than 64 distinct fields";
            spans[spanCount++] = {at, size};
        }
    }
    return nullptr;
}
} // namespace

const char* smgNativeBcsvProblem(const void* table) {
    const auto* header = static_cast<const u32*>(table);
    if (plausible(header[0], header[1], header[2], header[3])) return nullptr;
    if (!plausible(__builtin_bswap32(header[0]), __builtin_bswap32(header[1]), __builtin_bswap32(header[2]), __builtin_bswap32(header[3])))
        return "reads as neither byte order's";
    Span spans[64];
    u32 spanCount;
    return spansOf(header, spans, spanCount);
}

void smgNativeBcsv(void* table) {
    auto* header = static_cast<u32*>(table);
    if (const char* problem = smgNativeBcsvProblem(table)) stop(problem);
    if (plausible(header[0], header[1], header[2], header[3])) return;
    Span spans[64];
    u32 spanCount;
    spansOf(header, spans, spanCount);
    const u32 count = __builtin_bswap32(header[0]), fields = __builtin_bswap32(header[1]);
    const u32 offset = __builtin_bswap32(header[2]), stride = __builtin_bswap32(header[3]);
    auto* items = reinterpret_cast<JMapItem*>(header + 4);
    header[0] = count;
    header[1] = fields;
    header[2] = offset;
    header[3] = stride;
    for (u32 i = 0; i < fields; ++i) {
        items[i].mHash = __builtin_bswap32(items[i].mHash);
        items[i].mMask = __builtin_bswap32(items[i].mMask);
        items[i].mOffsData = __builtin_bswap16(items[i].mOffsData);
    }
    u8* records = static_cast<u8*>(table) + offset;
    for (u32 row = 0; row < count; ++row) {
        for (u32 s = 0; s < spanCount; ++s) {
            u8* value = records + row * stride + spans[s].at;
            if (spans[s].size == 4) *reinterpret_cast<u32*>(value) = __builtin_bswap32(*reinterpret_cast<u32*>(value));
            else *reinterpret_cast<u16*>(value) = __builtin_bswap16(*reinterpret_cast<u16*>(value));
        }
    }
}
