#include <smg/Bcsv.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

namespace smg {
namespace {
std::uint16_t be16(const std::uint8_t* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
template<typename T> void store(std::uint8_t* destination, T value) { std::memcpy(destination, &value, sizeof(value)); }
bool fail(std::string& error, const char* reason) { error = reason; return false; }
}

bool Bcsv::load(Bytes source, std::string& error) {
    info_.mData = nullptr;
    native_.clear(); error.clear();
    if (!source.data || source.size < 16 || source.size > INT32_MAX) return fail(error, "Invalid BCSV size");
    const auto count = be32(source.data);
    const auto fields = be32(source.data+4);
    const std::size_t offset = be32(source.data+8), stride = be32(source.data+12);
    if (count > INT32_MAX || fields > INT32_MAX || fields > (source.size-16)/12 ||
        offset > source.size || offset < 16 + fields*12 ||
        (count && (!stride || count > (source.size-offset)/stride)))
        return fail(error, "Invalid BCSV table bounds");
    const auto strings = offset + count*stride;
    struct Field { std::size_t offset, width, nativeOffset; unsigned type; };
    std::vector<Field> layout;
    std::size_t nativeStride = 0;
    for (std::size_t i=0; i<fields; ++i) {
        const auto* record = source.data + 16 + i*12;
        const std::size_t fieldOffset = be16(record+8);
        const unsigned type = record[11], shift = record[10];
        const std::size_t width = type==1 ? 32 : type==4 ? 2 : type==5 ? 1 : 4;
        const std::size_t alignment = type==1 ? 1 : width;
        if (type > 6 || fieldOffset > stride || width > stride-fieldOffset ||
            (type!=1 && shift >= width*8)) return fail(error, "Invalid BCSV field type, bounds or shift");
        std::size_t nativeOffset = (nativeStride+alignment-1)/alignment*alignment;
        for (const auto& field : layout) {
            if (fieldOffset < field.offset+field.width && field.offset < fieldOffset+width) {
                if (fieldOffset != field.offset || width != field.width || type != field.type)
                    return fail(error, "Incompatible overlapping BCSV fields");
                nativeOffset = field.nativeOffset;
            }
        }
        if (nativeOffset > UINT16_MAX) return fail(error,"Repacked BCSV field offset exceeds 16 bits");
        nativeStride = std::max(nativeStride,nativeOffset+width);
        layout.push_back({fieldOffset,width,nativeOffset,type});
    }
    // Some original animation tables put string offsets after six bytes of flags.
    // Repack all records so original JMapInfo's typed loads are aligned on ARM.
    nativeStride = (nativeStride+3)/4*4;
    const std::size_t nativeRecords = 16+fields*12;
    const std::size_t stringBytes = source.size-strings;
    if (stringBytes > INT32_MAX-nativeRecords ||
        (count && nativeStride > (INT32_MAX-nativeRecords-stringBytes)/count))
        return fail(error,"Repacked BCSV size overflow");
    const std::size_t nativeStrings = nativeRecords+count*nativeStride;
    const std::size_t nativeSize = nativeStrings+stringBytes;
    std::vector<std::uint32_t> native((nativeSize+3)/4);
    auto* bytes = reinterpret_cast<std::uint8_t*>(native.data());
    store(bytes,count); store(bytes+4,fields);
    store(bytes+8,static_cast<std::uint32_t>(nativeRecords));
    store(bytes+12,static_cast<std::uint32_t>(nativeStride));
    std::memcpy(bytes+nativeStrings,source.data+strings,stringBytes);
    for (std::size_t i=0;i<fields;++i) {
        const auto at=16+i*12;
        store(bytes+at,be32(source.data+at)); store(bytes+at+4,be32(source.data+at+4));
        store(bytes+at+8,static_cast<std::uint16_t>(layout[i].nativeOffset));
        bytes[at+10] = source.data[at+10]; bytes[at+11] = source.data[at+11];
    }
    for (std::size_t row=0;row<count;++row) {
        for (const auto& field : layout) {
            const auto at=offset+row*stride+field.offset;
            const auto to=nativeRecords+row*nativeStride+field.nativeOffset;
            if (field.type==1) {
                if (!std::memchr(source.data+at,0,32)) return fail(error,"Unterminated inline BCSV string");
                std::memcpy(bytes+to,source.data+at,32);
            } else if (field.type==6) {
                const auto stringOffset=be32(source.data+at);
                if (stringOffset >= source.size-strings ||
                    !std::memchr(source.data+strings+stringOffset,0,source.size-strings-stringOffset))
                    return fail(error,"Invalid BCSV string-table reference");
                store(bytes+to,stringOffset);
            } else if (field.width==4) store(bytes+to,be32(source.data+at));
            else if (field.width==2) store(bytes+to,be16(source.data+at));
            else bytes[to] = source.data[at];
        }
    }
    native_.swap(native);
    info_.attach(native_.data());
    return true;
}
} // namespace smg
