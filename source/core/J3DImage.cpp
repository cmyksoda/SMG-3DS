#include <smg/J3DImage.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DModelData.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphLoader/J3DJointFactory.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace smg {
namespace {
static_assert(sizeof(J3DJointInitData) == 0x40, "JNT1 joint record");

struct Reader {
    const std::uint8_t* d;
    std::size_t size;
    bool has(std::size_t at, std::size_t bytes) const { return at <= size && bytes <= size - at; }
    std::uint16_t u16(std::size_t at) const { return static_cast<std::uint16_t>(d[at] << 8 | d[at + 1]); }
    std::uint32_t u32(std::size_t at) const { return std::uint32_t(u16(at)) << 16 | u16(at + 2); }
    float f32(std::size_t at) const {
        const std::uint32_t bits = u32(at);
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
};

// Appends to the image, eight-byte aligned so the next struct needs no more.
struct Writer {
    std::vector<std::uint8_t> bytes;
    std::size_t reserve(std::size_t size) {
        const std::size_t at = bytes.size();
        bytes.resize(at + ((size + 7) & ~std::size_t(7)), 0);
        return at;
    }
    template <typename T> T* at(std::size_t offset) { return reinterpret_cast<T*>(bytes.data() + offset); }
};

bool fail(std::string& error, const char* reason) {
    error = reason;
    return false;
}

void* offsetField(std::size_t offset) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(offset)); }

// The value of the multi-character constant J3D compares a tag with, such as tag("INF1").
constexpr u32 tag(const char (&name)[5]) {
    return u32(std::uint8_t(name[0])) << 24 | u32(std::uint8_t(name[1])) << 16 | u32(std::uint8_t(name[2])) << 8 | std::uint8_t(name[3]);
}
} // namespace

bool J3DSkeletonImage::load(Bytes file, std::string& error) {
    const Reader r{file.data, file.size};
    if (!file.data || file.size < 0x20 || std::memcmp(file.data, "J3D2", 4)) return fail(error, "Not a J3D2 model");
    std::size_t inf = 0, jnt = 0, evp = 0, drw = 0;
    std::size_t at = 0x20;
    for (std::uint32_t i = 0, count = r.u32(12); i < count; ++i) {
        if (!r.has(at, 8)) return fail(error, "Truncated J3D block");
        const std::size_t size = r.u32(at + 4);
        if (size < 8 || !r.has(at, size)) return fail(error, "Invalid J3D block size");
        if (!std::memcmp(file.data + at, "INF1", 4)) inf = at;
        if (!std::memcmp(file.data + at, "JNT1", 4)) jnt = at;
        if (!std::memcmp(file.data + at, "EVP1", 4)) evp = at;
        if (!std::memcmp(file.data + at, "DRW1", 4)) drw = at;
        at += size;
    }
    if (!inf || !jnt || !drw) return fail(error, "J3D model without INF1, JNT1 or DRW1");
    const auto inside = [&](std::size_t block, std::size_t offset, std::size_t bytes) {
        return r.has(block, r.u32(block + 4)) && offset <= r.u32(block + 4) && bytes <= r.u32(block + 4) - offset;
    };

    Writer w;
    const std::size_t header = w.reserve(offsetof(J3DModelFileData, mBlocks));
    std::uint32_t blocks = 0;
    const auto finish = [&](std::size_t start, u32 type) {
        auto* block = w.at<J3DModelBlock>(start);
        block->mBlockType = type;
        block->mBlockSize = static_cast<u32>(w.bytes.size() - start);
        ++blocks;
    };

    // INF1: the joint nodes and the nesting around them. A removed material or shape leaves an empty level.
    {
        if (!r.has(inf, 24)) return fail(error, "Truncated INF1");
        std::vector<std::uint16_t> nodes;
        std::size_t node = inf + r.u32(inf + 20);
        for (;; node += 4) {
            if (!inside(inf, node - inf, 4)) return fail(error, "INF1 hierarchy runs past its block");
            const std::uint16_t type = r.u16(node);
            if (type == 0x11 || type == 0x12) continue;
            nodes.push_back(type);
            nodes.push_back(r.u16(node + 2));
            if (!type) break;
        }
        const std::size_t start = w.reserve(sizeof(J3DModelInfoBlock));
        const std::size_t hierarchy = w.reserve(nodes.size() * 2);
        std::memcpy(w.bytes.data() + hierarchy, nodes.data(), nodes.size() * 2);
        auto* block = w.at<J3DModelInfoBlock>(start);
        block->mFlags = r.u16(inf + 8);
        block->mPacketNum = r.u32(inf + 12);
        block->mVtxNum = r.u32(inf + 16);
        block->mpHierarchy = offsetField(hierarchy - start);
        finish(start, tag("INF1"));
    }

    // JNT1: each joint record swapped field by field, the index table, and the name table.
    {
        if (!r.has(jnt, 24)) return fail(error, "Truncated JNT1");
        const std::size_t count = r.u16(jnt + 8);
        const std::size_t records = jnt + r.u32(jnt + 12), indices = jnt + r.u32(jnt + 16), names = r.u32(jnt + 20);
        if (!count || !inside(jnt, records - jnt, count * 0x40) || !inside(jnt, indices - jnt, count * 2))
            return fail(error, "JNT1 tables run past its block");
        joints_ = count;
        const std::size_t start = w.reserve(sizeof(J3DJointBlock));
        const std::size_t data = w.reserve(count * sizeof(J3DJointInitData));
        const std::size_t table = w.reserve(count * 2);
        for (std::size_t j = 0; j < count; ++j) {
            const std::size_t in = records + j * 0x40;
            J3DJointInitData& out = w.at<J3DJointInitData>(data)[j];
            out.mKind = r.u16(in);
            out.mScaleCompensate = file.data[in + 2];
            out.mTransformInfo.mScale = {r.f32(in + 4), r.f32(in + 8), r.f32(in + 12)};
            out.mTransformInfo.mRotation = {static_cast<s16>(r.u16(in + 16)), static_cast<s16>(r.u16(in + 18)),
                                            static_cast<s16>(r.u16(in + 20))};
            out.mTransformInfo.mTranslate = {r.f32(in + 24), r.f32(in + 28), r.f32(in + 32)};
            out.mRadius = r.f32(in + 36);
            out.mMin = {r.f32(in + 40), r.f32(in + 44), r.f32(in + 48)};
            out.mMax = {r.f32(in + 52), r.f32(in + 56), r.f32(in + 60)};
            const std::uint16_t index = r.u16(indices + j * 2);
            if (index >= count) return fail(error, "JNT1 index table names a joint it does not have");
            w.at<std::uint16_t>(table)[j] = index;
        }
        std::size_t nameTable = 0;
        if (names) {
            const std::size_t in = jnt + names;
            if (!inside(jnt, names, 4)) return fail(error, "JNT1 name table runs past its block");
            const std::size_t entries = r.u16(in);
            const std::size_t size = r.u32(jnt + 4) - names;
            if (entries * 4 + 4 > size) return fail(error, "JNT1 name table runs past its block");
            nameTable = w.reserve(size);
            std::uint8_t* out = w.bytes.data() + nameTable;
            std::memcpy(out, file.data + in, size); // the names are bytes; the counts and offsets are swapped
            for (std::size_t i = 0; i < 2 + entries * 2; ++i) {
                const std::uint16_t v = r.u16(in + i * 2);
                std::memcpy(out + i * 2, &v, 2);
            }
        }
        auto* block = w.at<J3DJointBlock>(start);
        block->mJointNum = static_cast<u16>(count);
        block->mpJointInitData = offsetField(data - start);
        block->mpIndexTable = offsetField(table - start);
        block->mpNameTable = nameTable ? offsetField(nameTable - start) : nullptr;
        finish(start, tag("JNT1"));
    }

    // EVP1: each envelope's joint count, the joints and weights, and every joint's inverse bind.
    if (evp) {
        if (!r.has(evp, 28)) return fail(error, "Truncated EVP1");
        const std::size_t count = r.u16(evp + 8);
        const std::size_t mixes = r.u32(evp + 12), indices = r.u32(evp + 16), weights = r.u32(evp + 20), inverses = r.u32(evp + 24);
        if (!inside(evp, mixes, count)) return fail(error, "EVP1 tables run past its block");
        std::size_t total = 0, needed = 0;
        for (std::size_t e = 0; e < count; ++e) total += file.data[evp + mixes + e];
        if (!inside(evp, indices, total * 2) || !inside(evp, weights, total * 4)) return fail(error, "EVP1 tables run past its block");
        for (std::size_t i = 0; i < total; ++i) needed = std::max<std::size_t>(needed, r.u16(evp + indices + i * 2) + 1);
        // The loader reads an inverse bind only for a joint an envelope names; the disc keeps up to one per joint.
        const std::size_t available = inverses <= r.u32(evp + 4) ? (r.u32(evp + 4) - inverses) / 48 : 0;
        const std::size_t kept = std::min(joints_, available);
        if (needed > joints_) return fail(error, "EVP1 names a joint JNT1 does not have");
        if (kept < needed) return fail(error, "EVP1 lacks an inverse bind an envelope needs");
        const std::size_t start = w.reserve(sizeof(J3DEnvelopeBlock));
        const std::size_t mixOut = w.reserve(count), indexOut = w.reserve(total * 2), weightOut = w.reserve(total * 4);
        const std::size_t inverseOut = w.reserve(kept * 48);
        std::memcpy(w.bytes.data() + mixOut, file.data + evp + mixes, count);
        for (std::size_t i = 0; i < total; ++i) {
            w.at<std::uint16_t>(indexOut)[i] = r.u16(evp + indices + i * 2);
            w.at<float>(weightOut)[i] = r.f32(evp + weights + i * 4);
        }
        for (std::size_t i = 0; i < kept * 12; ++i) w.at<float>(inverseOut)[i] = r.f32(evp + inverses + i * 4);
        auto* block = w.at<J3DEnvelopeBlock>(start);
        block->mWEvlpMtxNum = static_cast<u16>(count);
        block->mpWEvlpMixMtxNum = offsetField(mixOut - start);
        block->mpWEvlpMixIndex = offsetField(indexOut - start);
        block->mpWEvlpMixWeight = offsetField(weightOut - start);
        block->mpInvJointMtx = offsetField(inverseOut - start);
        finish(start, tag("EVP1"));
    }

    // DRW1: each draw matrix's kind (a joint, or an envelope) and index.
    {
        if (!r.has(drw, 20)) return fail(error, "Truncated DRW1");
        const std::size_t count = r.u16(drw + 8), flags = r.u32(drw + 12), indices = r.u32(drw + 16);
        if (!inside(drw, flags, count) || !inside(drw, indices, count * 2)) return fail(error, "DRW1 tables run past its block");
        const std::size_t start = w.reserve(sizeof(J3DDrawBlock));
        const std::size_t flagOut = w.reserve(count), indexOut = w.reserve(count * 2);
        std::memcpy(w.bytes.data() + flagOut, file.data + drw + flags, count);
        for (std::size_t i = 0; i < count; ++i) w.at<std::uint16_t>(indexOut)[i] = r.u16(drw + indices + i * 2);
        auto* block = w.at<J3DDrawBlock>(start);
        block->mMtxNum = static_cast<u16>(count);
        block->mpDrawMtxFlag = offsetField(flagOut - start);
        block->mpDrawMtxIndex = offsetField(indexOut - start);
        finish(start, tag("DRW1"));
    }

    auto* head = w.at<J3DModelFileData>(header);
    head->mMagic1 = tag("J3D2");
    head->mMagic2 = tag("bmd3");
    head->mBlockNum = blocks;
    storage_.assign((w.bytes.size() + 7) / 8, 0);
    std::memcpy(storage_.data(), w.bytes.data(), w.bytes.size());
    error.clear();
    return true;
}

void forgetJ3DSkeleton(J3DModel* model) {
    J3DMtxBuffer* buffer = model->mMtxBuffer;
    J3DModelData* data = model->getModelData();
    delete[] buffer->mpScaleFlagArr;
    delete[] buffer->mpAnmMtx; // mpUserAnmMtx is the same array
    delete[] buffer->mpEvlpScaleFlagArr;
    delete[] buffer->mpWeightEvlpMtx;
    for (int i = 0; i < 2; ++i) {
        if (data->getDrawMtxNum() == 0 || !buffer->mpDrawMtxArr[i]) continue;
        delete[] buffer->mpDrawMtxArr[i][0];
        delete[] buffer->mpNrmMtxArr[i][0];
        delete[] buffer->mpDrawMtxArr[i];
        delete[] buffer->mpNrmMtxArr[i];
    }
    delete buffer;
    J3DJointTree& tree = data->getJointTree();
    for (u16 j = 0; j < tree.getJointNum(); ++j) delete tree.mJointNodePointer[j];
    delete[] tree.mJointNodePointer;
    delete tree.mJointName;
    delete[] tree.mWEvlpImportantMtxIdx;
    delete tree.mBasicMtxCalc;
    delete data;
    delete model;
}
} // namespace smg
