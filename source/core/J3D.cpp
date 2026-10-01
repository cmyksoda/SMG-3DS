// J3D model files turned native for JSystem's loaders (include/smg/J3D.hpp). Each block is walked as J3DModelLoader
// and its factories read it, through the structs they overlay (J3DJointInitData, J3DShapeInitData,
// J3DMaterialInitData, ResTIMG...); tables are sized by the indices the loader follows, never by guesses.
#include <smg/J3D.hpp>
#include <smg/SwapPlan.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace smg {
namespace {
constexpr std::uint32_t tag(const char (&text)[5]) {
    return std::uint32_t(std::uint8_t(text[0])) << 24 | std::uint32_t(std::uint8_t(text[1])) << 16 |
           std::uint32_t(std::uint8_t(text[2])) << 8 | std::uint8_t(text[3]);
}

struct Block {
    std::size_t start, end;
    std::size_t at(std::size_t offset) const { return start + offset; }
};

// JUTNameTab's ResNTAB: a count, then per name a hash and an offset from the table.
void nameTable(SwapPlan& plan, const Block& b, std::uint32_t offset) {
    if (!offset) return;
    const std::size_t at = b.at(offset);
    if (!plan.need(at, 4, b.end, "a name table")) return;
    const std::uint32_t count = plan.be16(at);
    if (!plan.need(at + 4, 4 * std::size_t(count), b.end, "a name table's entries")) return;
    plan.halves(at, 2 + 2 * std::size_t(count));
    for (std::uint32_t i = 0; i < count && plan.ok(); ++i) plan.name(at + plan.be16(at + 6 + 4 * i), b.end, "a name");
}

// INF1: flags, packet and vertex counts, and the hierarchy's (type, value) pairs up to type 0.
void information(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x18, b.end, "an INF1 header")) return;
    plan.half(b.at(8)), plan.words(b.at(0xC), 3);
    for (std::size_t at = b.at(plan.be32(b.at(0x14))); plan.ok(); at += 4) {
        if (!plan.need(at, 4, b.end, "the hierarchy")) return;
        plan.halves(at, 2);
        if (plan.be16(at) == 0) return;
    }
}

// VTX1: the attribute formats, then each array up to the next (or the block's end), by its components' type.
void vertices(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x40, b.end, "a VTX1 header")) return;
    plan.words(b.at(8), 14);
    struct Format { std::uint32_t count, type; };
    std::map<std::uint32_t, Format> formats;
    for (std::size_t at = b.at(plan.be32(b.at(8))); plan.ok(); at += 16) {
        if (!plan.need(at, 16, b.end, "vertex formats")) return;
        plan.words(at, 3), plan.keep(at + 12, 4);  // GXVtxAttrFmtList: three int-sized enums and a byte
        const std::uint32_t attr = plan.be32(at);
        if (attr == 0xFF) break;
        formats[attr] = {plan.be32(at + 4), plan.be32(at + 8)};
    }
    // mpVtxPosArray, Nrm, NBT, Color[2], TexCoord[8] as GX_VA_POS (9), NRM (10), NBT (25), CLR0 (11)...
    const std::uint32_t attrs[13] = {9, 10, 25, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    std::vector<std::pair<std::size_t, std::uint32_t>> arrays;
    for (int i = 0; i < 13; ++i)
        if (const std::uint32_t offset = plan.be32(b.at(0xC + 4 * i))) arrays.push_back({b.at(offset), attrs[i]});
    std::sort(arrays.begin(), arrays.end());
    for (std::size_t i = 0; i < arrays.size() && plan.ok(); ++i) {
        const auto [start, attr] = arrays[i];
        const std::size_t end = i + 1 < arrays.size() ? arrays[i + 1].first : b.end;
        const auto found = formats.find(attr);
        if (found == formats.end()) return void(plan.fail("has a vertex array (attribute %lu) without a format", static_cast<unsigned long>(attr)));
        const Format f = found->second;
        if (!plan.fits(start, 0, end)) return void(plan.fail("has a vertex array past its block"));
        if (attr == 11 || attr == 12) {
            // Colours: only RGB565 and RGBA4 are halves; the rest are bytes the GPU reads in order.
            if (f.type == 0 || f.type == 3) plan.halves(start, (end - start) / 2);
            continue;
        }
        if (f.type > 4) return void(plan.fail("has a vertex component type %lu", static_cast<unsigned long>(f.type)));
        const unsigned components = attr == 9 ? 2 + f.count : attr == 10 ? (f.count ? 9 : 3) : attr == 25 ? 9 : 1 + f.count;
        const unsigned size = f.type == 4 ? 4 : f.type >= 2 ? 2 : 1;
        const std::size_t units = (end - start) / (components * size) * components;
        if (size == 4) plan.words(start, units);
        else if (size == 2) plan.halves(start, units);
    }
}

// EVP1: per weighted matrix its mix count, then the mixes' joint indices and weights, and the inverse bind matrices.
void envelopes(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x1C, b.end, "an EVP1 header")) return;
    const std::uint32_t count = plan.be16(b.at(8));
    plan.half(b.at(8)), plan.words(b.at(0xC), 4);
    const std::uint32_t mixCounts = plan.be32(b.at(0xC)), indices = plan.be32(b.at(0x10)), weights = plan.be32(b.at(0x14)),
                        inverse = plan.be32(b.at(0x18));
    std::size_t mixes = 0;
    if (count && mixCounts) {
        if (!plan.need(b.at(mixCounts), count, b.end, "envelope mix counts")) return;
        for (std::uint32_t i = 0; i < count; ++i) mixes += plan.be8(b.at(mixCounts + i));
    }
    // The indices follow the count bytes unpadded, so they can start at an odd address.
    if (mixes && indices && plan.need(b.at(indices), 2 * mixes, b.end, "envelope indices")) plan.looseHalves(b.at(indices), mixes);
    if (mixes && weights && plan.need(b.at(weights), 4 * mixes, b.end, "envelope weights")) plan.words(b.at(weights), mixes);
    if (inverse && plan.fits(b.at(inverse), 0, b.end)) plan.words(b.at(inverse), (b.end - b.at(inverse)) / 48 * 12);
}

// DRW1: per drawn matrix a flag byte and a u16 index.
void draws(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x14, b.end, "a DRW1 header")) return;
    const std::uint32_t count = plan.be16(b.at(8));
    plan.half(b.at(8)), plan.words(b.at(0xC), 2);
    const std::uint32_t indices = plan.be32(b.at(0x10));
    if (count && indices && plan.need(b.at(indices), 2 * std::size_t(count), b.end, "draw indices")) plan.looseHalves(b.at(indices), count);
}

std::uint32_t highestIndex(SwapPlan& plan, std::size_t at, std::uint32_t count, std::size_t end, const char* what) {
    std::uint32_t highest = 0;
    if (!plan.need(at, 2 * std::size_t(count), end, what)) return 0;
    for (std::uint32_t i = 0; i < count; ++i) highest = std::max(highest, plan.be16(at + 2 * i));
    plan.halves(at, count);
    return highest;
}

// JNT1: J3DJointInitData per distinct joint (0x40 bytes): kind, a byte, J3DTransformInfo, radius and bounds.
void joints(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x18, b.end, "a JNT1 header")) return;
    const std::uint32_t count = plan.be16(b.at(8));
    plan.half(b.at(8)), plan.words(b.at(0xC), 3);
    if (!count) return;
    const std::uint32_t distinct = highestIndex(plan, b.at(plan.be32(b.at(0x10))), count, b.end, "the joint index table") + 1;
    const std::size_t init = b.at(plan.be32(b.at(0xC)));
    if (!plan.need(init, 0x40 * std::size_t(distinct), b.end, "joints")) return;
    for (std::uint32_t i = 0; i < distinct; ++i) {
        const std::size_t j = init + 0x40 * i;
        plan.half(j), plan.words(j + 4, 3), plan.halves(j + 0x10, 4), plan.words(j + 0x18, 10);
    }
    nameTable(plan, b, plan.be32(b.at(0x14)));
}

// SHP1: J3DShapeInitData per distinct shape, its vertex descriptor lists (GXVtxDescList: two int-sized enums, up to
// GX_VA_NULL), matrix and draw init data, and the matrix table. Display lists stay the GPU's.
void shapes(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x2C, b.end, "an SHP1 header")) return;
    const std::uint32_t count = plan.be16(b.at(8));
    plan.half(b.at(8)), plan.words(b.at(0xC), 8);
    if (!count) return;
    const std::uint32_t distinct = highestIndex(plan, b.at(plan.be32(b.at(0x10))), count, b.end, "the shape index table") + 1;
    const std::size_t init = b.at(plan.be32(b.at(0xC)));
    if (!plan.need(init, 0x28 * std::size_t(distinct), b.end, "shapes")) return;
    std::set<std::uint32_t> descLists;
    std::uint32_t mtxInits = 0, drawInits = 0;
    for (std::uint32_t i = 0; i < distinct; ++i) {
        const std::size_t s = init + 0x28 * i;
        plan.halves(s + 2, 4), plan.words(s + 0xC, 7);
        const std::uint32_t groups = plan.be16(s + 2);
        descLists.insert(plan.be16(s + 4));
        mtxInits = std::max(mtxInits, plan.be16(s + 6) + groups);
        drawInits = std::max(drawInits, plan.be16(s + 8) + groups);
    }
    const std::size_t descs = b.at(plan.be32(b.at(0x18)));
    for (std::uint32_t list : descLists) {
        for (std::size_t at = descs + list; plan.ok(); at += 8) {
            if (!plan.need(at, 8, b.end, "a vertex descriptor list")) return;
            plan.words(at, 2);
            if (plan.be32(at) == 0xFF) break;
        }
    }
    const std::size_t mtxInit = b.at(plan.be32(b.at(0x24)));
    if (!plan.need(mtxInit, 8 * std::size_t(mtxInits), b.end, "shape matrix groups")) return;
    std::uint32_t tableEnd = 0;
    for (std::uint32_t i = 0; i < mtxInits; ++i) {
        const std::size_t m = mtxInit + 8 * i;
        plan.halves(m, 2), plan.word(m + 4);
        tableEnd = std::max(tableEnd, plan.be32(m + 4) + plan.be16(m + 2));
    }
    const std::size_t table = b.at(plan.be32(b.at(0x1C)));
    if (tableEnd && plan.need(table, 2 * std::size_t(tableEnd), b.end, "the shape matrix table")) plan.halves(table, tableEnd);
    const std::size_t drawInit = b.at(plan.be32(b.at(0x28)));
    if (plan.need(drawInit, 8 * std::size_t(drawInits), b.end, "shape draws")) plan.words(drawInit, 2 * std::size_t(drawInits));
    nameTable(plan, b, plan.be32(b.at(0x14)));
}

// A table the materials index: each entry `size` bytes, turned by `entry` for every index they use.
template <typename Entry>
void table(SwapPlan& plan, const Block& b, std::uint32_t offset, const std::set<std::uint32_t>& used, std::size_t size, const char* what, Entry entry) {
    if (used.empty()) return;
    if (!offset) return void(plan.fail("has materials using a missing %s table", what));
    for (std::uint32_t index : used) {
        const std::size_t at = b.at(offset) + size * index;
        if (!plan.need(at, size, b.end, what)) return;
        entry(at);
    }
}

// J3DTexMtxInfo (0x64): four bytes, the centre, J3DTextureSRTInfo (with an s16 rotation), the effect matrix.
void textureMatrix(SwapPlan& plan, std::size_t at) {
    plan.words(at + 4, 3), plan.words(at + 0x10, 2), plan.half(at + 0x18), plan.words(at + 0x1C, 2), plan.words(at + 0x24, 16);
}

// MAT3: J3DMaterialInitData per distinct material (0x14C: bytes, then u16 indices), and the value tables they index.
void materials(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x84, b.end, "a MAT3 header")) return;
    const std::uint32_t count = plan.be16(b.at(8));
    plan.half(b.at(8)), plan.words(b.at(0xC), 30);
    if (!count) return;
    const auto offset = [&](int field) { return plan.be32(b.at(0xC + 4 * field)); };
    const std::uint32_t distinct = highestIndex(plan, b.at(offset(1)), count, b.end, "the material ID table") + 1;
    const std::size_t init = b.at(offset(0));
    if (!plan.need(init, 0x14C * std::size_t(distinct), b.end, "materials")) return;
    std::set<std::uint32_t> cull, texMtx, texNo, tevColor, fog, nbt;
    const auto use = [&](std::set<std::uint32_t>& into, std::size_t at, int n) {
        for (int i = 0; i < n; ++i)
            if (plan.be16(at + 2 * i) != 0xFFFF) into.insert(plan.be16(at + 2 * i));
    };
    for (std::uint32_t i = 0; i < distinct; ++i) {
        const std::size_t m = init + 0x14C * i;
        plan.halves(m + 8, 70), plan.halves(m + 0x94, 4), plan.halves(m + 0xBC, 68), plan.halves(m + 0x144, 4);
        if (plan.be8(m + 1) != 0xFF) cull.insert(plan.be8(m + 1));
        use(texMtx, m + 0x48, 8), use(texNo, m + 0x84, 8), use(tevColor, m + 0xDC, 4), use(fog, m + 0x144, 1), use(nbt, m + 0x14A, 1);
    }
    table(plan, b, offset(4), cull, 4, "cull mode", [&](std::size_t at) { plan.word(at); });  // GXCullMode, int-sized
    table(plan, b, offset(13), texMtx, 0x64, "texture matrix", [&](std::size_t at) { textureMatrix(plan, at); });
    table(plan, b, offset(15), texNo, 2, "texture number", [&](std::size_t at) { plan.half(at); });
    table(plan, b, offset(17), tevColor, 8, "TEV colour", [&](std::size_t at) { plan.halves(at, 4); });  // GXColorS10
    table(plan, b, offset(23), fog, 0x2C, "fog", [&](std::size_t at) { plan.half(at + 2), plan.words(at + 4, 4), plan.halves(at + 0x18, 10); });
    table(plan, b, offset(29), nbt, 0x10, "NBT scale", [&](std::size_t at) { plan.words(at + 4, 3); });
    // J3DIndInitData per material, read by index when the block has it (the factory's own test).
    const std::uint32_t indirect = offset(3), names = offset(2);
    if (indirect && indirect - names > 4) {
        if (!plan.need(b.at(indirect), 0x138 * std::size_t(count), b.end, "indirect texturing")) return;
        for (std::uint32_t i = 0; i < count; ++i)
            for (int k = 0; k < 3; ++k) plan.words(b.at(indirect) + 0x138 * i + 0x14 + 0x1C * k, 6);
    }
    nameTable(plan, b, names);
}

// TEX1: ResTIMG per texture (0x20); texels and palettes stay the GPU's.
void textures(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x14, b.end, "a TEX1 header")) return;
    const std::uint32_t count = plan.be16(b.at(8));
    plan.half(b.at(8)), plan.words(b.at(0xC), 2);
    const std::size_t headers = b.at(plan.be32(b.at(0xC)));
    if (count && !plan.need(headers, 0x20 * std::size_t(count), b.end, "texture headers")) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t t = headers + 0x20 * i;
        plan.halves(t + 2, 2), plan.half(t + 0xA), plan.word(t + 0xC), plan.half(t + 0x1A), plan.word(t + 0x1C);
    }
    nameTable(plan, b, plan.be32(b.at(0x10)));
}

// MDL3 (J3DMaterialDLBlock): per material its display list's offset and size, patching offsets, current matrix
// registers and mode byte; the display lists stay the GPU's.
void materialDisplayLists(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x24, b.end, "an MDL3 header")) return;
    const std::size_t count = plan.be16(b.at(8));
    plan.half(b.at(8)), plan.words(b.at(0xC), 6);
    const std::size_t lists = b.at(plan.be32(b.at(0xC))), patches = b.at(plan.be32(b.at(0x10))), matrices = b.at(plan.be32(b.at(0x14)));
    if (!plan.need(lists, 8 * count, b.end, "material display lists") || !plan.need(patches, 0x10 * count, b.end, "patching offsets") ||
        !plan.need(matrices, 8 * count, b.end, "current matrices"))
        return;
    plan.words(lists, 2 * count), plan.words(matrices, 2 * count);
    for (std::size_t i = 0; i < count; ++i) plan.halves(patches + 0x10 * i, 6);
    nameTable(plan, b, plan.be32(b.at(0x20)));
}
} // namespace

namespace {
// An array the block's header points at, `count` units long; nothing when the offset is null.
void floats(SwapPlan& plan, const Block& b, std::uint32_t offset, std::size_t count, const char* what) {
    if (offset && count && plan.need(b.at(offset), 4 * count, b.end, what)) plan.words(b.at(offset), count);
}
void shorts(SwapPlan& plan, const Block& b, std::uint32_t offset, std::size_t count, const char* what) {
    if (offset && count && plan.need(b.at(offset), 2 * count, b.end, what)) plan.halves(b.at(offset), count);
}
// Tables of J3DAnmKeyTableBase (max frame, offset, type: three halves each), `count` entries of `halves` each.
void keyTables(SwapPlan& plan, const Block& b, std::uint32_t offset, std::size_t count, std::size_t halves, const char* what) {
    shorts(plan, b, offset, count * halves, what);
}

// ANK1 (J3DAnmTransformKeyData): per joint three J3DAnmTransformKeyTables, then scale, rotation and translation keys.
void transformKeys(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x24, b.end, "an ANK1 header")) return;
    plan.halves(b.at(0xA), 5), plan.words(b.at(0x14), 4);
    const auto o = [&](std::size_t at) { return plan.be32(b.at(at)); };
    keyTables(plan, b, o(0x14), plan.be16(b.at(0xC)) * 3, 9, "joint key tables");
    floats(plan, b, o(0x18), plan.be16(b.at(0xE)), "scale keys");
    shorts(plan, b, o(0x1C), plan.be16(b.at(0x10)), "rotation keys");
    floats(plan, b, o(0x20), plan.be16(b.at(0x12)), "translation keys");
}

// TTK1 (J3DAnmTextureSRTKeyData): tracks (three per texture matrix), the materials and matrices they move, centres,
// keys; then the same again for post-texture matrices.
void textureKeys(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x60, b.end, "a TTK1 header")) return;
    plan.halves(b.at(0xA), 5), plan.words(b.at(0x14), 8), plan.halves(b.at(0x34), 4), plan.words(b.at(0x3C), 9);
    const auto o = [&](std::size_t at) { return plan.be32(b.at(at)); };
    for (const std::size_t base : {std::size_t(0xC), std::size_t(0x34)}) {
        const bool post = base == 0x34;
        const std::size_t tracks = plan.be16(b.at(base)), matrices = tracks / 3;
        keyTables(plan, b, o(post ? 0x3C : 0x14), tracks, 9, "texture key tables");
        shorts(plan, b, o(post ? 0x40 : 0x18), matrices, "texture animation materials");
        nameTable(plan, b, o(post ? 0x44 : 0x1C));
        floats(plan, b, o(post ? 0x4C : 0x24), 3 * matrices, "texture matrix centres");
        floats(plan, b, o(post ? 0x50 : 0x28), plan.be16(b.at(base + 2)), "texture scale keys");
        shorts(plan, b, o(post ? 0x54 : 0x2C), plan.be16(b.at(base + 4)), "texture rotation keys");
        floats(plan, b, o(post ? 0x58 : 0x30), plan.be16(b.at(base + 6)), "texture translation keys");
    }
}

// TRK1 (J3DAnmTevRegKeyData): colour and konst register tables (four key tables and a register byte each), their
// materials and names, and each channel's keys.
void tevRegisterKeys(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x58, b.end, "a TRK1 header")) return;
    plan.halves(b.at(0xA), 11), plan.words(b.at(0x20), 14);
    const auto o = [&](std::size_t at) { return plan.be32(b.at(at)); };
    for (int k = 0; k < 2; ++k) {
        const std::size_t count = plan.be16(b.at(0xC + 2 * k)), table = o(0x20 + 4 * k);
        if (count && table) {
            if (!plan.need(b.at(table), 0x1C * count, b.end, "TEV register tables")) return;
            for (std::size_t i = 0; i < count; ++i) plan.halves(b.at(table) + 0x1C * i, 12);
        }
        shorts(plan, b, o(0x28 + 4 * k), count, "TEV register materials");
        nameTable(plan, b, o(0x30 + 4 * k));
        for (int c = 0; c < 4; ++c) shorts(plan, b, o(0x38 + 0x10 * k + 4 * c), plan.be16(b.at(0x10 + 8 * k + 2 * c)), "TEV register keys");
    }
}

// PAK1 (J3DAnmColorKeyData): per material a J3DAnmColorKeyTable, then each channel's keys.
void colorKeys(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x34, b.end, "a PAK1 header")) return;
    plan.halves(b.at(0xC), 6), plan.words(b.at(0x18), 7);
    const auto o = [&](std::size_t at) { return plan.be32(b.at(at)); };
    const std::size_t count = plan.be16(b.at(0xE));
    keyTables(plan, b, o(0x18), count, 12, "colour key tables");
    shorts(plan, b, o(0x1C), count, "colour animation materials");
    nameTable(plan, b, o(0x20));
    for (int c = 0; c < 4; ++c) shorts(plan, b, o(0x24 + 4 * c), plan.be16(b.at(0x10 + 2 * c)), "colour keys");
}

// TPT1 (J3DAnmTexPatternFullData): per material its frames' run (max frame, offset, a texture byte, a half), then the
// texture numbers every run reads.
void texturePatterns(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x20, b.end, "a TPT1 header")) return;
    plan.halves(b.at(0xA), 3), plan.words(b.at(0x10), 4);
    const auto o = [&](std::size_t at) { return plan.be32(b.at(at)); };
    const std::size_t count = plan.be16(b.at(0xC)), table = o(0x10);
    std::size_t values = 0;
    if (count && table) {
        if (!plan.need(b.at(table), 8 * count, b.end, "texture pattern tables")) return;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t t = b.at(table) + 8 * i;
            plan.halves(t, 2), plan.half(t + 6);
            values = std::max<std::size_t>(values, std::size_t(plan.be16(t + 2)) + plan.be16(t));
        }
    }
    shorts(plan, b, o(0x14), values, "texture numbers");
    shorts(plan, b, o(0x18), count, "texture pattern materials");
    nameTable(plan, b, o(0x1C));
}

// VAF1 (J3DAnmVisibilityFullData): per shape its frames' run; the visibility bytes stay.
void visibility(SwapPlan& plan, const Block& b) {
    if (!plan.need(b.start, 0x18, b.end, "a VAF1 header")) return;
    plan.halves(b.at(0xA), 3), plan.words(b.at(0x10), 2);
    shorts(plan, b, plan.be32(b.at(0x10)), 2 * std::size_t(plan.be16(b.at(0xC))), "visibility tables");
}
} // namespace

const char* nativeJ3DAnimation(std::uint8_t* data, std::size_t size) {
    SwapPlan plan(data, size);
    if (!plan.is(0, "J3D1")) return "is not a J3D1 file";
    if (size < 0x20) return "is shorter than its header";
    plan.words(0, 4), plan.word(0x1C);
    const std::uint32_t blocks = plan.be32(0xC);
    std::size_t at = 0x20;
    for (std::uint32_t i = 0; i < blocks && plan.ok(); ++i) {
        if (!plan.need(at, 8, size, "a block header")) break;
        const std::uint32_t kind = plan.be32(at), length = plan.be32(at + 4);
        if (length < 8) return "has a block shorter than its header";
        plan.words(at, 2);
        // Nintendo's tool counted some TTK1 blocks a word past the file; nothing reads beyond the file itself.
        const Block b{at, std::min<std::size_t>(at + length, size)};
        switch (kind) {
        case tag("ANK1"): transformKeys(plan, b); break;
        case tag("TTK1"): textureKeys(plan, b); break;
        case tag("TRK1"): tevRegisterKeys(plan, b); break;
        case tag("PAK1"): colorKeys(plan, b); break;
        case tag("TPT1"): texturePatterns(plan, b); break;
        case tag("VAF1"): visibility(plan, b); break;
        default: plan.fail("has an animation block the port doesn't know (0x%08lx)", static_cast<unsigned long>(kind));
        }
        at += length;
    }
    return plan.apply();
}

const char* nativeJ3DModel(std::uint8_t* data, std::size_t size) {
    SwapPlan plan(data, size);
    if (!plan.is(0, "J3D2")) return "is not a J3D2 file";
    if (!plan.is(4, "bmd3") && !plan.is(4, "bdl4") && !plan.is(4, "bmt3")) return "is a J3D2 kind the port doesn't turn";
    if (size < 0x20) return "is shorter than its header";
    plan.words(0, 2), plan.word(0xC);
    const std::uint32_t blocks = plan.be32(0xC);
    std::size_t at = 0x20;
    for (std::uint32_t i = 0; i < blocks && plan.ok(); ++i) {
        if (!plan.need(at, 8, size, "a block header")) break;
        const std::uint32_t kind = plan.be32(at), length = plan.be32(at + 4);
        if (length < 8 || !plan.need(at, length, size, "a block")) break;
        plan.words(at, 2);
        const Block b{at, at + length};
        switch (kind) {
        case tag("INF1"): information(plan, b); break;
        case tag("VTX1"): vertices(plan, b); break;
        case tag("EVP1"): envelopes(plan, b); break;
        case tag("DRW1"): draws(plan, b); break;
        case tag("JNT1"): joints(plan, b); break;
        case tag("SHP1"): shapes(plan, b); break;
        case tag("MAT3"): materials(plan, b); break;
        case tag("TEX1"): textures(plan, b); break;
        case tag("MDL3"): materialDisplayLists(plan, b); break;
        default: plan.fail("has a block the port doesn't know (0x%08lx)", static_cast<unsigned long>(kind));
        }
        at += length;
    }
    return plan.apply();
}
} // namespace smg
