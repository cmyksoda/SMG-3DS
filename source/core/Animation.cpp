#include <smg/Animation.hpp>
#include <cmath>
#include <cstring>

namespace smg {
namespace {
std::uint16_t be16(const std::uint8_t* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
bool fail(std::string& error, const char* reason) { error = reason; return false; }

// A key table reads one value (one key), or keys of three (frame, value, tangent) or four (with an in and
// an out tangent), from its offset on.
bool validKeys(const J3DAnmKeyTableBase& table, std::size_t count) {
    if (table.mMaxFrame == 0) return true;
    if (table.mMaxFrame == 1) return table.mOffset < count;
    return std::size_t(table.mOffset) + std::size_t(table.mMaxFrame) * (table.mType == 0 ? 3 : 4) <= count;
}

J3DAnmKeyTableBase keyTable(const std::uint8_t* p) {
    return {be16(p), be16(p + 2), be16(p + 4)};
}

// A JUTNameTab: an entry count, then (key code, offset) pairs, offsets from the table's start.
bool names(const std::uint8_t* block, std::size_t size, std::size_t at, std::size_t count, std::vector<std::string>& out) {
    if (!count) return true;
    if (at > size || size - at < 4) return false;
    const std::uint8_t* table = block + at;
    if (be16(table) != count || (count * 4) > size - at - 4) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t offset = be16(table + 4 + i * 4 + 2);
        if (offset >= size - at || !std::memchr(table + offset, 0, size - at - offset)) return false;
        out.emplace_back(reinterpret_cast<const char*>(table + offset));
    }
    return true;
}
} // namespace

bool TevRegAnimation::load(Bytes source, std::string& error) {
    const std::uint8_t* d = source.data;
    if (!d || source.size < 0x20 || std::memcmp(d, "J3D1brk1", 8)) return fail(error, "Not a J3D1 BRK file");
    const std::uint32_t blocks = be32(d + 12);
    std::size_t at = 0x20;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        if (at > source.size || source.size - at < 8) return fail(error, "Truncated BRK block");
        const std::uint8_t* block = d + at;
        const std::size_t size = be32(block + 4);
        if (size < 8 || size > source.size - at) return fail(error, "Invalid BRK block size");
        if (std::memcmp(block, "TRK1", 4)) { at += size; continue; }
        if (size < 0x58) return fail(error, "Truncated TRK1 header");

        const std::size_t colorCount = be16(block + 0x0c), konstCount = be16(block + 0x0e);
        std::size_t counts[8];
        for (int c = 0; c < 8; ++c) counts[c] = be16(block + 0x10 + c * 2);
        const auto region = [&](std::size_t offset, std::size_t bytes) { return offset <= size && bytes <= size - offset; };
        const std::size_t colorTable = be32(block + 0x20), konstTable = be32(block + 0x24);
        if (!region(colorTable, colorCount * 0x1c) || !region(konstTable, konstCount * 0x1c))
            return fail(error, "TRK1 key tables outside the block");
        for (int c = 0; c < 8; ++c) {
            const std::size_t offset = be32(block + 0x38 + c * 4);
            if (!region(offset, counts[c] * 2)) return fail(error, "TRK1 values outside the block");
            values_[c].resize(counts[c]);
            for (std::size_t v = 0; v < counts[c]; ++v) values_[c][v] = static_cast<s16>(be16(block + offset + v * 2));
        }
        const auto readTables = [&](auto& tables, std::size_t offset, std::size_t count, int firstValues) {
            tables.resize(count);
            for (std::size_t t = 0; t < count; ++t) {
                const std::uint8_t* p = block + offset + t * 0x1c;
                auto& table = tables[t];
                table.mRTable = keyTable(p), table.mGTable = keyTable(p + 6), table.mBTable = keyTable(p + 12);
                table.mATable = keyTable(p + 18);
                table.mColorId = p[0x18];
                const J3DAnmKeyTableBase* channels[4]{&table.mRTable, &table.mGTable, &table.mBTable, &table.mATable};
                for (int c = 0; c < 4; ++c)
                    if (!validKeys(*channels[c], values_[firstValues + c].size())) return false;
                if (table.mColorId > 3) return false;
            }
            return true;
        };
        if (!readTables(colorTables_, colorTable, colorCount, 0) || !readTables(konstTables_, konstTable, konstCount, 4))
            return fail(error, "TRK1 key table names values it does not have");
        if (!names(block, size, be32(block + 0x30), colorCount, colorNames_) ||
            !names(block, size, be32(block + 0x34), konstCount, konstNames_))
            return fail(error, "Invalid TRK1 material names");

        // J3DAnmKeyLoader_v15::setAnmTevReg, pointing at the native copies.
        key_.mFrameMax = static_cast<s16>(be16(block + 0x0a));
        key_.mAttribute = block[8];
        key_.mFrame = 0.0f;
        key_.mCRegUpdateMaterialNum = static_cast<u16>(colorCount);
        key_.mKRegUpdateMaterialNum = static_cast<u16>(konstCount);
        key_.mAnmCRegKeyTable = colorTables_.data();
        key_.mAnmKRegKeyTable = konstTables_.data();
        s16** data[8]{&key_.mAnmCRegDataR, &key_.mAnmCRegDataG, &key_.mAnmCRegDataB, &key_.mAnmCRegDataA,
                      &key_.mAnmKRegDataR, &key_.mAnmKRegDataG, &key_.mAnmKRegDataB, &key_.mAnmKRegDataA};
        u16* countsOut[8]{&key_.mCRegDataCountR, &key_.mCRegDataCountG, &key_.mCRegDataCountB, &key_.mCRegDataCountA,
                          &key_.mKRegDataCountR, &key_.mKRegDataCountG, &key_.mKRegDataCountB, &key_.mKRegDataCountA};
        for (int c = 0; c < 8; ++c) *data[c] = values_[c].data(), *countsOut[c] = static_cast<u16>(counts[c]);
        error.clear();
        return true;
    }
    return fail(error, "BRK has no TRK1 block");
}

bool JointAnimation::load(Bytes source, std::string& error) {
    const std::uint8_t* d = source.data;
    if (!d || source.size < 0x20 || std::memcmp(d, "J3D1bck1", 8)) return fail(error, "Not a J3D1 BCK file");
    const std::uint32_t blocks = be32(d + 12);
    std::size_t at = 0x20;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        if (at > source.size || source.size - at < 8) return fail(error, "Truncated BCK block");
        const std::uint8_t* block = d + at;
        const std::size_t size = be32(block + 4);
        if (size < 8 || size > source.size - at) return fail(error, "Invalid BCK block size");
        if (std::memcmp(block, "ANK1", 4)) { at += size; continue; }
        if (size < 0x24) return fail(error, "Truncated ANK1 header");
        const std::size_t joints = be16(block + 0x0c), counts[3]{be16(block + 0x0e), be16(block + 0x10), be16(block + 0x12)};
        const std::size_t tables = be32(block + 0x14), offsets[3]{be32(block + 0x18), be32(block + 0x1c), be32(block + 0x20)};
        const auto region = [&](std::size_t offset, std::size_t bytes) { return offset <= size && bytes <= size - offset; };
        if (!region(tables, joints * 3 * 18) || !region(offsets[0], counts[0] * 4) || !region(offsets[1], counts[1] * 2) ||
            !region(offsets[2], counts[2] * 4))
            return fail(error, "ANK1 tables outside the block");
        scale_.resize(counts[0]), rotation_.resize(counts[1]), translation_.resize(counts[2]);
        const auto real = [](const std::uint8_t* p) {
            const std::uint32_t bits = be32(p);
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
        };
        for (std::size_t v = 0; v < counts[0]; ++v) scale_[v] = real(block + offsets[0] + v * 4);
        for (std::size_t v = 0; v < counts[1]; ++v) rotation_[v] = static_cast<s16>(be16(block + offsets[1] + v * 2));
        for (std::size_t v = 0; v < counts[2]; ++v) translation_[v] = real(block + offsets[2] + v * 4);
        for (float v : scale_) if (!std::isfinite(v)) return fail(error, "Invalid ANK1 scale");
        for (float v : translation_) if (!std::isfinite(v)) return fail(error, "Invalid ANK1 translation");
        tables_.resize(joints * 3);
        for (std::size_t t = 0; t < tables_.size(); ++t) {
            const std::uint8_t* p = block + tables + t * 18;
            auto& table = tables_[t];
            table.mScaleInfo = keyTable(p), table.mRotationInfo = keyTable(p + 6), table.mTranslateInfo = keyTable(p + 12);
            if (!validKeys(table.mScaleInfo, counts[0]) || !validKeys(table.mRotationInfo, counts[1]) ||
                !validKeys(table.mTranslateInfo, counts[2]))
                return fail(error, "ANK1 key table names values it does not have");
        }
        // J3DAnmKeyLoader_v15::setAnmTransform, pointing at the native copies.
        key_.field_0x1e = static_cast<u16>(joints);
        key_.mFrameMax = static_cast<s16>(be16(block + 0x0a));
        key_.mAttribute = block[8];
        key_.mDecShift = block[9];
        key_.mFrame = 0.0f;
        key_.mAnmTable = tables_.data();
        key_.mScaleData = scale_.data();
        key_.mRotData = rotation_.data();
        key_.mTransData = translation_.data();
        error.clear();
        return true;
    }
    return fail(error, "BCK has no ANK1 block");
}

void JointAnimation::pose(float frame, std::vector<JointTransform>& pose) const {
    for (std::size_t j = 0; j < joints() && j < pose.size(); ++j) {
        J3DTransformInfo info;
        key_.calcTransform(frame, static_cast<u16>(j), &info);
        pose[j] = {{info.mScale.x, info.mScale.y, info.mScale.z}, {info.mRotation.x, info.mRotation.y, info.mRotation.z},
                   {info.mTranslate.x, info.mTranslate.y, info.mTranslate.z}};
    }
}

unsigned TevRegAnimation::apply(float frame, std::vector<ModelMaterial>& materials) {
    J3DAnmTevRegKey& key = key_;
    key.mFrame = frame;
    unsigned matched = 0;
    for (std::size_t i = 0; i < colorTables_.size(); ++i) {
        for (auto& material : materials) {
            if (material.name != colorNames_[i]) continue;
            GXColorS10 color;
            key.getTevColorReg(static_cast<u16>(i), &color);
            material.tevColors[colorTables_[i].mColorId] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
            ++matched;
        }
    }
    for (std::size_t i = 0; i < konstTables_.size(); ++i) {
        for (auto& material : materials) {
            if (material.name != konstNames_[i]) continue;
            GXColor color;
            key.getTevKonstReg(static_cast<u16>(i), &color);
            material.konstColors[konstTables_[i].mColorId] = {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f};
            ++matched;
        }
    }
    return matched;
}
} // namespace smg
