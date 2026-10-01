// resource_audit FILES --j3d: every BMD, BDL and BMT on the disc turned native, then read back through J3D's own
// structs (the joint, shape and material init data, texture matrices, fog, ResTIMG, GX's vertex lists) as its loader
// and factories read them, and compared with a big-endian read of the original.
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3DGraphLoader/J3DJointFactory.hpp>
#include <JSystem/J3DGraphLoader/J3DMaterialFactory.hpp>
#include <JSystem/J3DGraphLoader/J3DShapeFactory.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <smg/Archive.hpp>
#include <smg/J3D.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
using Bytes = std::vector<std::uint8_t>;
std::uint32_t be16(const std::uint8_t* at) { return std::uint32_t(at[0]) << 8 | at[1]; }
std::uint32_t be32(const std::uint8_t* at) { return be16(at) << 16 | be16(at + 2); }

struct Audit {
    std::size_t files = 0, refused = 0, fields = 0, wrong = 0, implausible = 0;
    std::map<std::string, std::size_t> counts;
    const std::uint8_t* before = nullptr;
    const std::uint8_t* after = nullptr;
    std::string where;

    std::size_t at(const void* field) const { return static_cast<const std::uint8_t*>(field) - after; }
    void same(std::uint32_t native, std::uint32_t expected, const char* what) {
        ++fields;
        if (native != expected && wrong++ < 20) std::printf("%s: %s reads 0x%x, big-endian 0x%x\n", where.c_str(), what, native, expected);
    }
    template <typename T> void field(const T& value, const char* what) {
        if constexpr (sizeof(T) == 4) {
            std::uint32_t native;
            std::memcpy(&native, &value, 4);
            same(native, be32(before + at(&value)), what);
        } else if constexpr (sizeof(T) == 2) {
            std::uint16_t native;
            std::memcpy(&native, &value, 2);
            same(native, be16(before + at(&value)), what);
        } else {
            static_assert(sizeof(T) == 1);
            same(*reinterpret_cast<const std::uint8_t*>(&value), before[at(&value)], what);
        }
    }
    // A u16 the file may place at an odd address, read without a reference to it.
    void looseHalf(const std::uint8_t* at, const char* what) {
        std::uint16_t native;
        std::memcpy(&native, at, 2);
        same(native, be16(before + (at - after)), what);
    }
    void plausible(bool ok, const char* what) {
        if (!ok && implausible++ < 20) std::printf("%s: %s is implausible\n", where.c_str(), what);
    }
    const std::uint8_t* block(std::uint32_t kind) const {
        std::size_t o = 0x20;
        for (std::uint32_t i = 0, n = be32(before + 0xC); i < n; ++i, o += be32(before + o + 4))
            if (be32(before + o) == kind) return after + o;
        return nullptr;
    }
    // A block's offset field, read natively from the converted copy (and checked against the original).
    std::uint32_t offset(const std::uint8_t* b, std::size_t field) {
        std::uint32_t value;
        std::memcpy(&value, b + field, 4);
        same(value, be32(before + at(b + field)), "a block offset");
        return value;
    }
    std::uint16_t count(const std::uint8_t* b) {
        std::uint16_t value;
        std::memcpy(&value, b + 8, 2);
        same(value, be16(before + at(b + 8)), "a block count");
        return value;
    }
};

constexpr std::uint32_t tag(const char (&t)[5]) {
    return std::uint32_t(std::uint8_t(t[0])) << 24 | std::uint32_t(std::uint8_t(t[1])) << 16 | std::uint32_t(std::uint8_t(t[2])) << 8 | std::uint8_t(t[3]);
}
bool finite(const Vec& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
template <typename T> const T* in(const std::uint8_t* base, std::uint32_t offset) { return reinterpret_cast<const T*>(base + offset); }

void names(Audit& a, const std::uint8_t* b, std::uint32_t offset) {
    if (!offset) return;
    const std::uint8_t* table = b + offset;
    std::uint16_t n;
    std::memcpy(&n, table, 2);
    a.same(n, be16(a.before + a.at(table)), "a name count");
    for (std::uint16_t i = 0; i < n; ++i) {
        std::uint16_t hash, at;
        std::memcpy(&hash, table + 4 + 4 * i, 2), std::memcpy(&at, table + 6 + 4 * i, 2);
        a.same(hash, be16(a.before + a.at(table + 4 + 4 * i)), "a name's hash");
        a.same(at, be16(a.before + a.at(table + 6 + 4 * i)), "a name's offset");
        // JUTNameTab's hash of the name, which the game looks names up by. Nintendo's tools hashed bytes past 0x7F
        // as signed chars; the game hashes them unsigned, so those names are never found by name on the Wii either.
        std::uint16_t computed = 0, signedHash = 0;
        for (const char* c = reinterpret_cast<const char*>(table + at); *c; ++c) {
            computed = static_cast<std::uint16_t>(computed * 3 + static_cast<std::uint8_t>(*c));
            signedHash = static_cast<std::uint16_t>(signedHash * 3 + static_cast<std::int8_t>(*c));
        }
        a.plausible(computed == hash || signedHash == hash, "a name's hash");
        if (computed != hash && signedHash == hash) ++a.counts["names hashed with signed chars"];
    }
}

void joints(Audit& a) {
    const std::uint8_t* b = a.block(tag("JNT1"));
    if (!b) return;
    const std::uint16_t n = a.count(b);
    const auto* init = in<J3DJointInitData>(b, a.offset(b, 0xC));
    const auto* index = in<std::uint16_t>(b, a.offset(b, 0x10));
    for (std::uint16_t i = 0; i < n; ++i) {
        a.field(index[i], "joint index");
        const J3DJointInitData& j = init[index[i]];
        a.field(j.mKind, "joint kind"), a.field(j.mScaleCompensate, "joint scale compensation");
        const J3DTransformInfo& t = j.mTransformInfo;
        a.field(t.mScale.x, "joint scale"), a.field(t.mScale.y, "joint scale"), a.field(t.mScale.z, "joint scale");
        a.field(t.mRotation.x, "joint rotation"), a.field(t.mRotation.y, "joint rotation"), a.field(t.mRotation.z, "joint rotation");
        a.field(t.mTranslate.x, "joint translation"), a.field(t.mTranslate.y, "joint translation"), a.field(t.mTranslate.z, "joint translation");
        a.field(j.mRadius, "joint radius"), a.field(j.mMin.x, "joint bounds"), a.field(j.mMax.z, "joint bounds");
        a.plausible(finite(t.mScale) && finite(t.mTranslate) && std::isfinite(j.mRadius) && j.mKind <= 2, "a joint");
        ++a.counts["joints"];
    }
    names(a, b, a.offset(b, 0x14));
}

void shapes(Audit& a) {
    const std::uint8_t* b = a.block(tag("SHP1"));
    if (!b) return;
    const std::uint16_t n = a.count(b);
    const auto* init = in<J3DShapeInitData>(b, a.offset(b, 0xC));
    const auto* index = in<std::uint16_t>(b, a.offset(b, 0x10));
    const std::uint8_t* descs = b + a.offset(b, 0x18);
    const auto* table = in<std::uint16_t>(b, a.offset(b, 0x1C));
    const auto* mtx = in<J3DShapeMtxInitData>(b, a.offset(b, 0x24));
    const auto* draw = in<J3DShapeDrawInitData>(b, a.offset(b, 0x28));
    const std::uint32_t displayLists = a.offset(b, 0x20), blockSize = be32(a.before + a.at(b) + 4);
    for (std::uint16_t i = 0; i < n; ++i) {
        const J3DShapeInitData& s = init[index[i]];
        a.field(s.mShapeMtxType, "shape matrix type"), a.field(s.mMtxGroupNum, "shape matrix groups"), a.field(s.mVtxDescListIndex, "shape descriptors");
        a.field(s.mMtxInitDataIndex, "shape matrix data"), a.field(s.mDrawInitDataIndex, "shape draw data"), a.field(s.mRadius, "shape radius");
        a.field(s.mMin.x, "shape bounds"), a.field(s.mMax.y, "shape bounds");
        a.plausible(s.mShapeMtxType <= 3 && std::isfinite(s.mRadius) && finite(s.mMin) && finite(s.mMax), "a shape");
        for (const auto* d = reinterpret_cast<const GXVtxDescList*>(descs + s.mVtxDescListIndex);; ++d) {
            a.field(d->attr, "a vertex descriptor's attribute"), a.field(d->type, "a vertex descriptor's type");
            if (d->attr == GX_VA_NULL) break;
            a.plausible((d->attr <= GX_VA_TEX7 || d->attr == GX_VA_NBT) && d->type <= GX_INDEX16, "a vertex descriptor");
        }
        for (std::uint16_t g = 0; g < s.mMtxGroupNum; ++g) {
            const J3DShapeMtxInitData& m = mtx[s.mMtxInitDataIndex + g];
            a.field(m.mUseMtxIndex, "a group's matrix"), a.field(m.mUseMtxCount, "a group's matrix count"), a.field(m.mFirstUseMtxIndex, "a group's first matrix");
            for (std::uint16_t k = 0; k < m.mUseMtxCount; ++k) a.field(table[m.mFirstUseMtxIndex + k], "a group's matrix table entry");
            const J3DShapeDrawInitData& d = draw[s.mDrawInitDataIndex + g];
            a.field(d.mDisplayListSize, "a display list's size"), a.field(d.mDisplayListIndex, "a display list's offset");
            a.plausible(displayLists + d.mDisplayListIndex + d.mDisplayListSize <= blockSize && d.mDisplayListSize % 32 == 0, "a display list");
            a.plausible(std::memcmp(b + displayLists + d.mDisplayListIndex, a.before + a.at(b) + displayLists + d.mDisplayListIndex, d.mDisplayListSize) == 0,
                        "a display list (it stays big-endian)");
        }
        ++a.counts["shapes"];
    }
    names(a, b, a.offset(b, 0x14));
}

void materials(Audit& a) {
    const std::uint8_t* b = a.block(tag("MAT3"));
    if (!b) return;
    const std::uint16_t n = a.count(b);
    std::uint32_t o[30];
    for (int i = 0; i < 30; ++i) o[i] = a.offset(b, 0xC + 4 * i);
    const auto* init = in<J3DMaterialInitData>(b, o[0]);
    const auto* ids = in<std::uint16_t>(b, o[1]);
    for (std::uint16_t i = 0; i < n; ++i) {
        a.field(ids[i], "material ID");
        const J3DMaterialInitData& m = init[ids[i]];
        a.field(m.mMaterialMode, "material mode"), a.field(m.mCullModeIdx, "cull index"), a.field(m.mTevStageNumIdx, "stage count index");
        for (int k = 0; k < 2; ++k) a.field(m.mMatColorIdx[k], "material colour index"), a.field(m.mAmbColorIdx[k], "ambient index");
        for (int k = 0; k < 4; ++k) a.field(m.mColorChanIdx[k], "channel index"), a.field(m.mTevKColorIdx[k], "konst index"),
                                   a.field(m.mTevColorIdx[k], "TEV colour index"), a.field(m.mTevSwapModeTableIdx[k], "swap table index");
        for (int k = 0; k < 8; ++k) a.field(m.mTexCoordIdx[k], "texgen index"), a.field(m.mTexMtxIdx[k], "texture matrix index"), a.field(m.mTexNoIdx[k], "texture index");
        for (int k = 0; k < 16; ++k) a.field(m.mTevKColorSel[k], "konst selection"), a.field(m.mTevOrderIdx[k], "TEV order index"),
                                    a.field(m.mTevStageIdx[k], "TEV stage index"), a.field(m.mTevSwapModeIdx[k], "swap index");
        a.field(m.mFogIdx, "fog index"), a.field(m.mAlphaCompIdx, "alpha test index"), a.field(m.mBlendIdx, "blend index"), a.field(m.mNBTScaleIdx, "NBT index");
        if (m.mCullModeIdx != 0xFF) {
            const auto& cull = in<GXCullMode>(b, o[4])[m.mCullModeIdx];
            a.field(cull, "cull mode"), a.plausible(cull <= GX_CULL_ALL, "a cull mode");
        }
        for (int k = 0; k < 8; ++k) {
            if (m.mTexMtxIdx[k] != 0xFFFF) {
                const J3DTexMtxInfo& t = in<J3DTexMtxInfo>(b, o[13])[m.mTexMtxIdx[k]];
                a.field(t.mProjection, "texture matrix projection"), a.field(t.mCenter.x, "texture matrix centre"), a.field(t.mSRT.mScaleX, "texture scale");
                a.field(t.mSRT.mRotation, "texture rotation"), a.field(t.mSRT.mTranslationY, "texture translation"), a.field(t.mEffectMtx[3][3], "effect matrix");
                a.plausible(std::isfinite(t.mSRT.mScaleX) && std::isfinite(t.mEffectMtx[0][0]), "a texture matrix");
            }
            if (m.mTexNoIdx[k] != 0xFFFF) a.field(in<std::uint16_t>(b, o[15])[m.mTexNoIdx[k]], "texture number");
        }
        for (int k = 0; k < 4; ++k)
            if (m.mTevColorIdx[k] != 0xFFFF) {
                const GXColorS10& c = in<GXColorS10>(b, o[17])[m.mTevColorIdx[k]];
                a.field(c.r, "TEV colour"), a.field(c.a, "TEV colour"), a.plausible(c.r >= -1024 && c.r < 1024, "a TEV colour");
            }
        if (m.mFogIdx != 0xFFFF) {
            const J3DFogInfo& f = in<J3DFogInfo>(b, o[23])[m.mFogIdx];
            a.field(f.mType, "fog type"), a.field(f.mCenter, "fog centre"), a.field(f.mStartZ, "fog start"), a.field(f.mFarZ, "fog far");
            a.field(f.mFogAdjTable.r[9], "fog adjustment");
        }
        if (m.mNBTScaleIdx != 0xFFFF) {
            const J3DNBTScaleInfo& s = in<J3DNBTScaleInfo>(b, o[29])[m.mNBTScaleIdx];
            a.field(s.mbHasScale, "NBT flag"), a.field(s.mScale.y, "NBT scale");
        }
        if (o[3] && o[3] - o[2] > 4) {
            const J3DIndInitData& d = in<J3DIndInitData>(b, o[3])[i];
            a.field(d.mEnabled, "indirect flag"), a.field(d.mIndTexStageNum, "indirect stages"), a.field(d.mIndTexMtxInfo[0].field_0x0[1][2], "indirect matrix");
        }
        ++a.counts["materials"];
    }
    names(a, b, o[2]);
}

void textures(Audit& a) {
    const std::uint8_t* b = a.block(tag("TEX1"));
    if (!b) return;
    const std::uint16_t n = a.count(b);
    const auto* t = in<ResTIMG>(b, a.offset(b, 0xC));
    for (std::uint16_t i = 0; i < n; ++i) {
        a.field(t[i].mFormat, "texture format"), a.field(t[i].mWidth, "texture width"), a.field(t[i].mHeight, "texture height"), a.field(t[i].mPaletteNum, "palette size");
        a.field(t[i].mPaletteDataOffset, "palette offset"), a.field(t[i].mLodBias, "LOD bias"), a.field(t[i].mImageDataOffset, "image offset");
        a.plausible(t[i].mWidth && t[i].mWidth <= 1024 && t[i].mHeight && t[i].mHeight <= 1024 && t[i].mFormat <= 14 && t[i].mFormat != 7, "a texture header");
        ++a.counts["textures"];
    }
    names(a, b, a.offset(b, 0x10));
}

void vertices(Audit& a) {
    const std::uint8_t* b = a.block(tag("VTX1"));
    if (!b) return;
    const auto* f = in<GXVtxAttrFmtList>(b, a.offset(b, 8));
    for (;; ++f) {
        a.field(f->attr, "a vertex format's attribute"), a.field(f->cnt, "a vertex format's count"), a.field(f->type, "a vertex format's type"), a.field(f->frac, "a vertex format's fraction");
        if (f->attr == GX_VA_NULL) break;
        a.plausible(f->type <= GX_F32 || f->attr == GX_VA_CLR0 || f->attr == GX_VA_CLR1, "a vertex format");
    }
    for (int i = 0; i < 13; ++i) a.offset(b, 0xC + 4 * i);
    // Positions read natively as their format says: the first and last complete element.
    const std::uint32_t positions = a.offset(b, 0xC);
    const auto* pf = in<GXVtxAttrFmtList>(b, a.offset(b, 8));
    while (pf->attr != GX_VA_NULL && pf->attr != GX_VA_POS) ++pf;
    if (positions && pf->attr == GX_VA_POS) {
        const std::uint8_t* p = b + positions;
        if (pf->type == GX_F32) {
            for (int k = 0; k < 3; ++k) a.field(reinterpret_cast<const float*>(p)[k], "a position");
            a.plausible(std::isfinite(reinterpret_cast<const float*>(p)[0]), "a position");
        } else if (pf->type == GX_S16) {
            for (int k = 0; k < 3; ++k) a.field(reinterpret_cast<const std::int16_t*>(p)[k], "a position");
        }
        ++a.counts["vertex arrays"];
    }
}

void envelopesAndDraws(Audit& a) {
    if (const std::uint8_t* b = a.block(tag("EVP1"))) {
        const std::uint16_t n = a.count(b);
        const std::uint32_t mix = a.offset(b, 0xC), index = a.offset(b, 0x10), weight = a.offset(b, 0x14);
        a.offset(b, 0x18);
        const std::uint32_t size = be32(a.before + a.at(b) + 4);
        std::size_t total = 0;
        if (mix && index && weight)
            for (std::uint16_t i = 0; i < n; ++i) total += b[mix + i];
        a.plausible(!total || (index + 2 * total <= size && weight + 4 * total <= size), "the envelope tables' extent");
        if (index + 2 * total > size || weight + 4 * total > size) total = 0;
        for (std::size_t i = 0; i < total; ++i) {
            a.looseHalf(b + index + 2 * i, "an envelope index");
            const float& w = in<float>(b, weight)[i];
            a.field(w, "an envelope weight"), a.plausible(w >= 0 && w <= 1.0001f, "an envelope weight");
        }
    }
    if (const std::uint8_t* b = a.block(tag("DRW1"))) {
        const std::uint16_t n = a.count(b);
        a.offset(b, 0xC);
        const std::uint32_t index = a.offset(b, 0x10);
        for (std::uint16_t i = 0; i < n; ++i) a.looseHalf(b + index + 2 * i, "a draw index");
    }
    if (const std::uint8_t* b = a.block(tag("MDL3"))) {
        const std::uint16_t n = a.count(b);
        const auto* lists = in<J3DDisplayListInit>(b, a.offset(b, 0xC));
        const auto* patches = in<J3DPatchingInfo>(b, a.offset(b, 0x10));
        a.offset(b, 0x14);
        for (std::uint16_t i = 0; i < n; ++i) {
            a.field(lists[i].mOffset, "a material list's offset"), a.field(lists[i].field_0x4, "a material list's size");
            a.field(patches[i].mTexNoOffset, "a texture patch offset"), a.field(patches[i].mFogOffset, "a fog patch offset");
        }
        a.counts["material display lists"] += n;
        names(a, b, a.offset(b, 0x20));
    }
}

void auditFile(Audit& a, const std::string& where, const std::uint8_t* data, std::size_t size) {
    Bytes copy(data, data + size);
    a.where = where;
    ++a.files;
    if (const char* why = smg::nativeJ3DModel(copy.data(), copy.size())) {
        ++a.refused;
        std::printf("%s: refused: %s\n", where.c_str(), why);
        return;
    }
    a.before = data, a.after = copy.data();
    joints(a), shapes(a), materials(a), textures(a), vertices(a), envelopesAndDraws(a);
    Bytes twice = copy;
    a.plausible(smg::nativeJ3DModel(twice.data(), twice.size()) != nullptr, "a second turn (it must be refused)");
}

// A key table's own fields, and that its keys lie inside the value array the header counts.
void keyTable(Audit& a, const J3DAnmKeyTableBase& t, std::uint32_t values, const char* what) {
    a.field(t.mMaxFrame, what), a.field(t.mOffset, what), a.field(t.mType, what);
    const std::uint32_t stride = t.mMaxFrame == 1 ? 1 : t.mType == 0 ? 3 : 4;
    a.plausible(t.mType <= 1 && t.mOffset + t.mMaxFrame * stride <= values + (t.mMaxFrame == 1 ? 0 : 1), what);
}

void animation(Audit& a, const std::string& where, const std::uint8_t* data, std::size_t size) {
    Bytes copy(data, data + size);
    a.where = where;
    ++a.files;
    if (const char* why = smg::nativeJ3DAnimation(copy.data(), copy.size())) {
        ++a.refused;
        std::printf("%s: refused: %s\n", where.c_str(), why);
        return;
    }
    a.before = data, a.after = copy.data();
    const std::uint8_t* b = copy.data() + 0x20;
    const auto half = [&](std::size_t at) { std::uint16_t v; std::memcpy(&v, b + at, 2); a.same(v, be16(a.before + a.at(b + at)), "a header half"); return v; };
    const auto word = [&](std::size_t at) { std::uint32_t v; std::memcpy(&v, b + at, 4); a.same(v, be32(a.before + a.at(b + at)), "a header word"); return v; };
    const std::uint32_t kind = be32(data + 0x20);
    if (kind == tag("ANK1")) {
        const std::uint16_t joints = half(0xC), scales = half(0xE), rotations = half(0x10), translations = half(0x12);
        const auto* tables = in<J3DAnmTransformKeyTable>(b, word(0x14));
        word(0x18), word(0x1C), word(0x20);
        for (std::uint32_t i = 0; i < joints * 3u; ++i) {
            keyTable(a, tables[i].mScaleInfo, scales, "a scale key table");
            keyTable(a, tables[i].mRotationInfo, rotations, "a rotation key table");
            keyTable(a, tables[i].mTranslateInfo, translations, "a translation key table");
        }
        if (scales) a.field(in<float>(b, word(0x18))[scales - 1], "the last scale key");
        if (rotations) a.field(in<std::int16_t>(b, word(0x1C))[rotations - 1], "the last rotation key");
        if (translations) a.field(in<float>(b, word(0x20))[0], "the first translation key");
        ++a.counts["BCK joint animations"];
    } else if (kind == tag("TTK1")) {
        const std::uint16_t tracks = half(0xC), scales = half(0xE), rotations = half(0x10), translations = half(0x12);
        const auto* tables = in<J3DAnmTransformKeyTable>(b, word(0x14));
        for (std::uint32_t i = 0; i < tracks; ++i) {
            keyTable(a, tables[i].mScaleInfo, scales, "a texture scale key table");
            keyTable(a, tables[i].mRotationInfo, rotations, "a texture rotation key table");
            keyTable(a, tables[i].mTranslateInfo, translations, "a texture translation key table");
        }
        for (std::uint32_t i = 0; i < tracks / 3u; ++i) a.field(in<std::uint16_t>(b, word(0x18))[i], "a texture animation's material");
        if (word(0x24) && tracks >= 3) a.field(in<Vec>(b, word(0x24))[0].z, "a texture matrix centre");
        ++a.counts["BTK texture animations"];
    } else if (kind == tag("TRK1")) {
        for (int k = 0; k < 2; ++k) {
            const std::uint16_t count = half(0xC + 2 * k);
            std::uint16_t values[4];
            for (int c = 0; c < 4; ++c) values[c] = half(0x10 + 8 * k + 2 * c);
            const auto* tables = in<J3DAnmCRegKeyTable>(b, word(0x20 + 4 * k));
            for (std::uint16_t i = 0; i < count; ++i) {
                keyTable(a, tables[i].mRTable, values[0], "a register key table"), keyTable(a, tables[i].mATable, values[3], "a register key table");
                a.field(tables[i].mColorId, "a register's number"), a.plausible(tables[i].mColorId < 4, "a register's number");
                a.field(in<std::uint16_t>(b, word(0x28 + 4 * k))[i], "a register animation's material");
            }
        }
        ++a.counts["BRK register animations"];
    } else if (kind == tag("PAK1")) {
        const std::uint16_t count = half(0xE);
        std::uint16_t values[4];
        for (int c = 0; c < 4; ++c) values[c] = half(0x10 + 2 * c);
        const auto* tables = in<J3DAnmColorKeyTable>(b, word(0x18));
        for (std::uint16_t i = 0; i < count; ++i) keyTable(a, tables[i].mRInfo, values[0], "a colour key table"), keyTable(a, tables[i].mAInfo, values[3], "a colour key table");
        ++a.counts["BPK colour animations"];
    } else if (kind == tag("TPT1")) {
        const std::uint16_t count = half(0xC);
        const auto* tables = in<J3DAnmTexPatternFullTable>(b, word(0x10));
        const std::uint32_t values = word(0x14);
        for (std::uint16_t i = 0; i < count; ++i) {
            a.field(tables[i].mMaxFrame, "a pattern's length"), a.field(tables[i].mOffset, "a pattern's start"), a.field(tables[i].mTexNo, "a pattern's texture");
            a.field(in<std::uint16_t>(b, values)[tables[i].mOffset], "a pattern's first texture");
        }
        ++a.counts["BTP texture patterns"];
    } else if (kind == tag("VAF1")) {
        const std::uint16_t count = half(0xC);
        const auto* tables = in<J3DAnmVisibilityFullTable>(b, word(0x10));
        for (std::uint16_t i = 0; i < count; ++i) a.field(tables[i].mMaxFrame, "a visibility run's length"), a.field(tables[i].mOffset, "a visibility run's start");
        ++a.counts["BVA visibility animations"];
    }
    Bytes twice = copy;
    a.plausible(smg::nativeJ3DAnimation(twice.data(), twice.size()) != nullptr, "a second turn (it must be refused)");
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

int auditJ3D(const fs::path& root) {
    Audit a;
    std::vector<fs::path> archives;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".arc") archives.push_back(entry.path());
    std::sort(archives.begin(), archives.end());
    for (const auto& path : archives) {
        Bytes raw(fs::file_size(path));
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) raw.resize(std::fread(raw.data(), 1, raw.size(), f)), std::fclose(f);
        Bytes storage;
        std::string error;
        smg::Bytes source{raw.data(), raw.size()};
        if (raw.size() >= 4 && std::memcmp(raw.data(), "Yaz0", 4) == 0) {
            if (!smg::decodeYaz0(source, storage, 64u << 20, error)) continue;
            source = {storage.data(), storage.size()};
        }
        std::vector<smg::ArchiveEntry> entries;
        if (!smg::indexRarc(source, entries, error)) continue;
        for (const auto& entry : entries) {
            const std::string name = lower(entry.path);
            const std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
            if ((ext == ".bmd" || ext == ".bdl" || ext == ".bmt") && entry.size >= 8 && std::memcmp(source.data + entry.offset, "J3D2", 4) == 0)
                auditFile(a, fs::relative(path, root).string() + " " + entry.path, source.data + entry.offset, entry.size);
            if (entry.size >= 0x28 && std::memcmp(source.data + entry.offset, "J3D1", 4) == 0)
                animation(a, fs::relative(path, root).string() + " " + entry.path, source.data + entry.offset, entry.size);
        }
    }
    for (const auto& [what, count] : a.counts) std::printf("  %s: %zu\n", what.c_str(), count);
    std::printf("%zu files, %zu refused, %zu fields compared, %zu differ, %zu implausible\n", a.files, a.refused, a.fields, a.wrong, a.implausible);
    return a.refused || a.wrong || a.implausible ? 1 : 0;
}
