#include <smg/Kcl.hpp>
#include <Game/Map/KCollision.hpp>
#include <cmath>
#include <cstring>

namespace smg {
namespace {
std::uint16_t be16(const std::uint8_t* p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
float beFloat(const std::uint8_t* p) {
    const std::uint32_t bits = be32(p);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
template <typename T> void store(std::uint8_t* destination, T value) { std::memcpy(destination, &value, sizeof value); }
bool fail(std::string& error, const char* reason) { error = reason; return false; }

constexpr std::size_t WiiHeader = 0x38, VectorSize = 12, PrismSize = 16;

// Walks the octree as KCollisionServer::searchBlock does, marking each byte as a node (u32) or part of a
// prism list (u16), so each is swapped as the game reads it and nothing reachable points outside the file.
struct Octree {
    enum : std::uint8_t { Unknown, Node, List };
    const std::uint8_t* file;
    std::size_t begin, end, prisms;
    std::vector<std::uint8_t> kind;

    bool mark(std::size_t at, std::size_t size, std::uint8_t what) {
        if (at < begin || at > end || size > end - at) return false;
        for (std::size_t i = 0; i < size; ++i) {
            auto& slot = kind[at - begin + i];
            if (slot != Unknown && slot != what) return false;
            slot = what;
        }
        return true;
    }

    // A node's value is relative to the block holding it: a branch leads to eight children, a leaf
    // (top bit set) to a zero-terminated list the game reads from its second u16. The skipped one is
    // whatever precedes the list, often the last node's low half, so it is never marked.
    bool node(std::size_t at, std::size_t base, int shift) {
        if (at < begin || at >= end || (at - begin) % 4) return false;
        const bool walked = kind[at - begin] == Node;
        if (!mark(at, 4, Node)) return false;
        if (walked) return true;
        const std::uint32_t value = be32(file + at);
        if (value & 0x80000000u) {
            const std::size_t list = base + (value & 0x7fffffffu);
            if (list < begin || (list - begin) % 2) return false;
            for (std::size_t entry = list + 2;; entry += 2) {
                if (!mark(entry, 2, List)) return false;
                const std::uint16_t index = be16(file + entry);
                if (!index) return true;
                if (index > prisms) return false;
            }
        }
        if (shift < 1 || value > end - base) return false;
        const std::size_t children = base + value;
        for (int child = 0; child < 8; ++child)
            if (!node(children + child * 4, children, shift - 1)) return false;
        return true;
    }
};

// A file's sections and parameters, checked so nothing the game can reach lies outside it, and its octree's bytes
// classed by the walk.
struct Layout {
    std::size_t positions = 0, normals = 0, prismBase = 0, octree = 0, positionCount = 0, normalCount = 0, prismCount = 0;
    float thickness = 0, minimum[3]{};
    std::uint32_t masks[3]{};
    std::int32_t shift = 0, xShift = 0, xyShift = 0;
    std::vector<std::uint8_t> kind;
};

const char* parse(Bytes source, Layout& l) {
    const std::uint8_t* d = source.data;
    if (!d || source.size < WiiHeader || source.size > INT32_MAX) return "Invalid KCL size";
    l.positions = be32(d), l.normals = be32(d + 4), l.prismBase = be32(d + 8), l.octree = be32(d + 12);
    // Prisms count from one: the array's slot zero overlaps the end of the normals.
    if (l.positions < WiiHeader || l.positions % 4 || l.normals < l.positions || l.prismBase + PrismSize < l.normals ||
        l.octree < l.prismBase + PrismSize || l.octree % 4 || l.octree > source.size || (l.normals - l.positions) % VectorSize ||
        (l.prismBase + PrismSize - l.normals) % VectorSize || (l.octree - l.prismBase - PrismSize) % PrismSize)
        return "Invalid KCL section offsets";
    l.positionCount = (l.normals - l.positions) / VectorSize;
    l.normalCount = (l.prismBase + PrismSize - l.normals) / VectorSize;
    l.prismCount = (l.octree - l.prismBase - PrismSize) / PrismSize;
    if (!l.prismCount || l.prismCount > 0xffff) return "Invalid KCL triangle count";

    l.thickness = beFloat(d + 0x10);
    for (int axis = 0; axis < 3; ++axis) l.minimum[axis] = beFloat(d + 0x14 + axis * 4), l.masks[axis] = be32(d + 0x20 + axis * 4);
    l.shift = static_cast<std::int32_t>(be32(d + 0x2c));
    l.xShift = static_cast<std::int32_t>(be32(d + 0x30)), l.xyShift = static_cast<std::int32_t>(be32(d + 0x34));
    if (!std::isfinite(l.thickness) || !std::isfinite(l.minimum[0]) || !std::isfinite(l.minimum[1]) || !std::isfinite(l.minimum[2]))
        return "Invalid KCL bounds";
    if (l.shift < 0 || l.shift > 31) return "Invalid KCL block width";
    std::uint32_t blocks[3];
    for (int axis = 0; axis < 3; ++axis) {
        const std::uint32_t extent = ~l.masks[axis];
        if (extent & (extent + 1)) return "Invalid KCL axis mask";
        blocks[axis] = (extent >> l.shift) + 1;
    }
    std::size_t topNodes = 1;
    if (l.xShift != -1 || l.xyShift != -1) {
        if (l.xShift < 0 || l.xyShift < l.xShift || l.xyShift > 31 || blocks[0] > (1u << l.xShift) || blocks[1] > (1u << (l.xyShift - l.xShift)))
            return "Invalid KCL block layout";
        topNodes = ((std::size_t(blocks[2] - 1) << l.xyShift) | (std::size_t(blocks[1] - 1) << l.xShift) | (blocks[0] - 1)) + 1;
    }
    if (topNodes > (source.size - l.octree) / 4) return "KCL octree is truncated";

    Octree tree{d, l.octree, source.size, l.prismCount, std::vector<std::uint8_t>(source.size - l.octree)};
    for (std::size_t i = 0; i < topNodes; ++i)
        if (!tree.node(l.octree + i * 4, l.octree, l.shift)) return "Invalid KCL octree node or prism list";

    for (std::size_t i = 0; i < (l.positionCount + l.normalCount) * 3; ++i)
        if (!std::isfinite(beFloat(d + l.positions + i * 4))) return "Invalid KCL vector";
    for (std::size_t i = 1; i <= l.prismCount; ++i) {
        const std::uint8_t* prism = d + l.prismBase + i * PrismSize;
        if (!std::isfinite(beFloat(prism)) || be16(prism + 4) >= l.positionCount) return "Invalid KCL triangle";
        for (int normal = 0; normal < 4; ++normal)
            if (be16(prism + 6 + normal * 2) >= l.normalCount) return "Invalid KCL triangle normal";
    }
    l.kind.swap(tree.kind);
    return nullptr;
}
} // namespace

bool Kcl::load(Bytes source, std::string& error) {
    storage_.clear();
    triangles_ = 0;
    error.clear();
    Layout l;
    if (const char* why = parse(source, l)) return fail(error, why);
    const std::uint8_t* d = source.data;
    const std::size_t positions = l.positions, prismBase = l.prismBase, octree = l.octree;
    const std::size_t positionCount = l.positionCount, normalCount = l.normalCount, prismCount = l.prismCount;
    const std::vector<std::uint8_t>& kind = l.kind;

    // Header in this build's own layout, then positions, normals, prisms (slot zero kept) and the octree.
    const std::size_t header = (sizeof(KCLFile) + 15) / 16 * 16;
    const std::size_t nativePositions = header, nativeNormals = nativePositions + positionCount * VectorSize;
    const std::size_t nativePrisms = nativeNormals + normalCount * VectorSize;
    const std::size_t nativeOctree = nativePrisms + (prismCount + 1) * PrismSize;
    const std::size_t size = nativeOctree + (source.size - octree);
    std::vector<std::uint64_t> storage((size + 7) / 8);
    auto* out = reinterpret_cast<std::uint8_t*>(storage.data());

    KCLFile file{};
    file.mPosOffset = static_cast<u32>(nativePositions);
    file.mNormOffset = static_cast<u32>(nativeNormals);
    file.mPrismOffset = static_cast<u32>(nativePrisms);
    file.mOctreeOffset = static_cast<u32>(nativeOctree);
    file.mThickness = l.thickness;
    file.mMin.set(l.minimum[0], l.minimum[1], l.minimum[2]);
    file.mXMask = static_cast<s32>(l.masks[0]);
    file.mYMask = static_cast<s32>(l.masks[1]);
    file.mZMask = static_cast<s32>(l.masks[2]);
    file.mBlockWidthShift = l.shift;
    file.mBlockXShift = l.xShift;
    file.mBlockXYShift = l.xyShift;
    std::memcpy(out, &file, sizeof file);

    for (std::size_t i = 0; i < (positionCount + normalCount) * 3; ++i)
        store(out + nativePositions + i * 4, beFloat(d + positions + i * 4));
    for (std::size_t i = 1; i <= prismCount; ++i) {
        const std::uint8_t* prism = d + prismBase + i * PrismSize;
        std::uint8_t* to = out + nativePrisms + i * PrismSize;
        store(to, beFloat(prism));
        for (int field = 0; field < 6; ++field) store(to + 4 + field * 2, be16(prism + 4 + field * 2));
    }
    // Bytes no node reaches are never read; they keep their disc order.
    for (std::size_t at = 0; at < kind.size();) {
        const std::uint8_t* from = d + octree + at;
        std::uint8_t* to = out + nativeOctree + at;
        if (kind[at] == Octree::Node) store(to, be32(from)), at += 4;
        else if (kind[at] == Octree::List) store(to, be16(from)), at += 2;
        else *to = *from, at += 1;
    }
    storage_.swap(storage);
    triangles_ = prismCount;
    return true;
}

const char* nativeKcl(std::uint8_t* data, std::size_t size) {
    Layout l;
    if (const char* why = parse({data, size}, l)) return why;
    const auto swap32 = [](std::uint8_t* p) { store(p, be32(p)); };
    for (std::size_t at = 0; at < WiiHeader; at += 4) swap32(data + at);
    for (std::size_t i = 0; i < (l.positionCount + l.normalCount) * 3; ++i) swap32(data + l.positions + i * 4);
    for (std::size_t i = 1; i <= l.prismCount; ++i) {
        std::uint8_t* prism = data + l.prismBase + i * PrismSize;
        swap32(prism);
        for (int field = 0; field < 6; ++field) store(prism + 4 + field * 2, be16(prism + 4 + field * 2));
    }
    // Bytes no node reaches are never read; they keep their disc order.
    for (std::size_t at = 0; at < l.kind.size();) {
        std::uint8_t* p = data + l.octree + at;
        if (l.kind[at] == Octree::Node) swap32(p), at += 4;
        else if (l.kind[at] == Octree::List) store(p, be16(p)), at += 2;
        else at += 1;
    }
    return nullptr;
}
} // namespace smg
