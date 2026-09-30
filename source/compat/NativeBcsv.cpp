// BCSV tables turned native as JMapInfo attaches them (compat/smg/NativeResources.hpp): in place where every field
// sits where ARM can load it, else as a repacked copy beside the original. A header reads as a plausible table
// in only one byte order, which says whether the table still needs turning.
#include <Game/Util/JMapInfo.hpp>
#include <smg/NativeResources.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void* (*smgBcsvAllocator)(const void* beside, u32 size) = [](const void*, u32 size) { return std::malloc(size); };

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

namespace {
u32 bigEndian32(const u8* at) { return u32(at[0]) << 24 | u32(at[1]) << 16 | u32(at[2]) << 8 | at[3]; }
u32 bigEndian16(const u8* at) { return u32(at[0]) << 8 | at[1]; }

// A native copy with each field at an offset its size divides, laid out as smg::Bcsv lays it out: fields in
// order, a field that shares another's place sharing its new place too, records a multiple of four long.
const void* repacked(const u8* table) {
    const u32 count = bigEndian32(table), fields = bigEndian32(table + 4), offset = bigEndian32(table + 8), stride = bigEndian32(table + 12);
    if (fields > 256) stop("has more fields than a repacked copy takes");
    u32 nativeOffsets[256], nativeStride = 0;
    for (u32 i = 0; i < fields; ++i) {
        const u8* item = table + 16 + i * 12;
        const u32 at = bigEndian16(item + 8), type = item[11], size = width(type), alignment = type == 1 ? 1 : size;
        u32 place = (nativeStride + alignment - 1) / alignment * alignment;
        for (u32 j = 0; j < i; ++j) {
            const u8* other = table + 16 + j * 12;
            const u32 otherAt = bigEndian16(other + 8), otherSize = width(other[11]);
            if (at < otherAt + otherSize && otherAt < at + size) {
                if (at != otherAt || size != otherSize || type != other[11]) stop("has overlapping fields of different widths");
                place = nativeOffsets[j];
            }
        }
        nativeOffsets[i] = place;
        if (place + size > nativeStride) nativeStride = place + size;
    }
    nativeStride = (nativeStride + 3) / 4 * 4;
    // The string table runs to the end of the file, whose size a table does not record: as far as its last string.
    const u8* strings = table + offset + count * stride;
    u32 stringBytes = 0;
    for (u32 i = 0; i < fields; ++i) {
        const u8* item = table + 16 + i * 12;
        if (item[11] != 6) continue;
        for (u32 row = 0; row < count; ++row) {
            const u32 at = bigEndian32(table + offset + row * stride + bigEndian16(item + 8));
            const u32 end = at + static_cast<u32>(std::strlen(reinterpret_cast<const char*>(strings + at))) + 1;
            if (end > stringBytes) stringBytes = end;
        }
    }
    const u32 records = 16 + fields * 12, size = records + count * nativeStride + stringBytes;
    auto* out = static_cast<u8*>(smgBcsvAllocator(table, size));
    if (!out) stop("could not be given memory for its repacked copy");
    auto* header = reinterpret_cast<u32*>(out);
    header[0] = count;
    header[1] = fields;
    header[2] = records;
    header[3] = nativeStride;
    for (u32 i = 0; i < fields; ++i) {
        const u8* item = table + 16 + i * 12;
        auto* copy = reinterpret_cast<JMapItem*>(out + 16 + i * 12);
        copy->mHash = bigEndian32(item);
        copy->mMask = bigEndian32(item + 4);
        copy->mOffsData = static_cast<u16>(nativeOffsets[i]);
        copy->mShift = item[10];
        copy->mType = item[11];
    }
    for (u32 row = 0; row < count; ++row) {
        for (u32 i = 0; i < fields; ++i) {
            const u8* item = table + 16 + i * 12;
            const u8* from = table + offset + row * stride + bigEndian16(item + 8);
            u8* to = out + records + row * nativeStride + nativeOffsets[i];
            const u32 size = width(item[11]);
            if (size == 32) std::memcpy(to, from, 32);
            else if (size == 4) *reinterpret_cast<u32*>(to) = bigEndian32(from);
            else if (size == 2) *reinterpret_cast<u16*>(to) = static_cast<u16>(bigEndian16(from));
            else *to = *from;
        }
    }
    std::memcpy(out + records + count * nativeStride, strings, stringBytes);
    return out;
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

const void* smgNativeBcsv(void* table) {
    auto* header = static_cast<u32*>(table);
    if (plausible(header[0], header[1], header[2], header[3])) return table;
    if (const char* problem = smgNativeBcsvProblem(table)) {
        if (std::strstr(problem, "repacking")) return repacked(static_cast<const u8*>(table));
        stop(problem);
    }
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
    return table;
}
