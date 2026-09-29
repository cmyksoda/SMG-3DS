#include <smg/Bcsv.hpp>
#include <cstring>
#include <limits>
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
        offset > source.size || offset < 16 + fields*12 || offset % 4 ||
        (count && (!stride || stride % 4 || count > (source.size-offset)/stride)))
        return fail(error, "Invalid BCSV table bounds or alignment");
    const auto strings = offset + count*stride;
    struct Field { std::size_t offset, width; unsigned type; };
    std::vector<Field> layout;
    for (std::size_t i=0; i<fields; ++i) {
        const auto* record = source.data + 16 + i*12;
        const std::size_t fieldOffset = be16(record+8);
        const unsigned type = record[11], shift = record[10];
        const std::size_t width = type==1 ? 32 : type==4 ? 2 : type==5 ? 1 : 4;
        const std::size_t alignment = type==1 ? 1 : width;
        if (type > 6 || fieldOffset > stride || width > stride-fieldOffset || fieldOffset % alignment ||
            (type!=1 && shift >= width*8)) return fail(error, "Invalid BCSV field type, bounds, shift or alignment");
        for (const auto& field : layout) {
            if (fieldOffset < field.offset+field.width && field.offset < fieldOffset+width &&
                !(fieldOffset == field.offset && width == field.width && type == field.type))
                return fail(error, "Incompatible overlapping BCSV fields");
        }
        layout.push_back({fieldOffset,width,type});
    }
    if (source.size > std::numeric_limits<std::size_t>::max()-3) return fail(error, "BCSV size overflow");
    std::vector<std::uint32_t> native((source.size+3)/4);
    auto* bytes = reinterpret_cast<std::uint8_t*>(native.data());
    std::memcpy(bytes,source.data,source.size);
    for (unsigned i=0;i<16;i+=4) store(bytes+i,be32(source.data+i));
    for (std::size_t i=0;i<fields;++i) {
        const auto at=16+i*12;
        store(bytes+at,be32(source.data+at)); store(bytes+at+4,be32(source.data+at+4));
        store(bytes+at+8,be16(source.data+at+8));
    }
    for (std::size_t row=0;row<count;++row) {
        for (const auto& field : layout) {
            const auto at=offset+row*stride+field.offset;
            if (field.type==1) {
                if (!std::memchr(source.data+at,0,32)) return fail(error,"Unterminated inline BCSV string");
            } else if (field.type==6) {
                const auto stringOffset=be32(source.data+at);
                if (stringOffset >= source.size-strings ||
                    !std::memchr(source.data+strings+stringOffset,0,source.size-strings-stringOffset))
                    return fail(error,"Invalid BCSV string-table reference");
                store(bytes+at,stringOffset);
            } else if (field.width==4) store(bytes+at,be32(source.data+at));
            else if (field.width==2) store(bytes+at,be16(source.data+at));
        }
    }
    native_.swap(native);
    info_.attach(native_.data());
    return true;
}
} // namespace smg
