#include <smg/J3D.hpp>
#include <smg/Jpa.hpp>
#include <smg/SwapPlan.hpp>

namespace smg {
namespace {
// One resource block (`at` its tag, `size` its length from there), by the layouts JParticle's blocks read.
void block(SwapPlan& plan, std::size_t at, std::size_t size, unsigned textureTable) {
    const std::size_t end = at + size;
    const auto need = [&](std::size_t length, const char* what) { return plan.need(at, length, end, what); };
    if (plan.is(at, "BEM1")) {  // JPADynamicsBlockData: flags and user word, nine vectors' floats, thirteen floats, eight halves
        if (need(0x7C, "a dynamics block")) plan.words(at + 8, 2), plan.words(at + 0x10, 9), plan.words(at + 0x34, 13), plan.halves(at + 0x68, 8);
    } else if (plan.is(at, "BSP1")) {  // JPABaseShapeData, then its texture-matrix and color key tables
        if (!need(0x34, "a base shape")) return;
        plan.word(at + 8), plan.halves(at + 0xC, 2), plan.words(at + 0x10, 2), plan.half(at + 0x18), plan.half(at + 0x24);
        if (plan.be32(at + 8) & 0x01000000 && need(0x34 + 0x28, "a texture matrix animation")) plan.words(at + 0x34, 10);
        // Color keys (JPAClrAnmKeyData): a frame half, then a GXColor's bytes.
        const std::uint32_t colors = plan.be8(at + 0x21);
        const struct { std::uint32_t flag; std::size_t offset, keys; } tables[2] = {
            {2, plan.be16(at + 0xC), plan.be8(at + 0x22)}, {8, plan.be16(at + 0xE), plan.be8(at + 0x23)}};
        for (const auto& t : tables)
            if (colors & t.flag && plan.need(at + t.offset, 6 * t.keys, end, "color keys"))
                for (std::size_t k = 0; k < t.keys; ++k) plan.half(at + t.offset + 6 * k);
    } else if (plan.is(at, "ESP1")) {  // JPAExtraShapeData: flags, seven floats, two halves, thirteen floats
        if (need(0x60, "an extra shape")) plan.word(at + 8), plan.words(at + 0xC, 7), plan.halves(at + 0x28, 2), plan.words(at + 0x2C, 13);
    } else if (plan.is(at, "SSP1")) {  // JPAChildShapeData: flags, ten floats, two colors, a float, two halves, two bytes, a half
        if (need(0x48, "a child shape")) plan.word(at + 8), plan.words(at + 0xC, 10), plan.word(at + 0x3C), plan.halves(at + 0x40, 2), plan.half(at + 0x46);
    } else if (plan.is(at, "ETX1")) {  // JPAExTexShapeData: flags and an indirect matrix
        if (need(0x28, "an extra texture shape")) plan.word(at + 8), plan.words(at + 0xC, 6);
    } else if (plan.is(at, "FLD1")) {  // JPAFieldBlockData: flags and thirteen floats
        if (need(0x44, "a field")) plan.word(at + 8), plan.words(at + 0xC, 13);
    } else if (plan.is(at, "KFA1")) {  // JPAKeyBlock: a key count at 9, then four floats a key
        if (need(0xC, "a key block") && need(0xC + std::size_t(16) * plan.be8(at + 9), "key frames")) plan.words(at + 0xC, 4 * std::size_t(plan.be8(at + 9)));
    } else if (plan.is(at, "TDB1")) {  // the resource's texture numbers
        if (need(8 + 2 * std::size_t(textureTable), "texture numbers")) plan.halves(at + 8, textureTable);
    } else {
        plan.fail("has a resource block the port doesn't know at 0x%zx", at);
    }
}
} // namespace

const char* nativeJpc(std::uint8_t* data, std::size_t size) {
    SwapPlan plan(data, size);
    if (!plan.is(0, "JPAC") || !plan.is(4, "2-10")) return "is not a JPAC 2-10 particle container";
    if (!plan.need(0, 0x10, size, "a header")) return plan.failed();
    plan.words(0, 2), plan.halves(8, 2), plan.word(0xC);
    const std::uint32_t resources = plan.be16(8), textures = plan.be16(0xA);
    std::size_t at = 0x10;
    for (std::uint32_t r = 0; r < resources && plan.ok(); ++r) {
        // JPAResourceHeader: user index, block count, field, key and texture-number counts.
        if (!plan.need(at, 8, size, "a resource header")) break;
        plan.halves(at, 2);
        const std::uint32_t blocks = plan.be16(at + 2), textureTable = plan.be8(at + 6);
        at += 8;
        for (std::uint32_t b = 0; b < blocks && plan.ok(); ++b) {
            if (!plan.need(at, 8, size, "a block header")) break;
            const std::size_t length = plan.be32(at + 4);
            if (length < 8 || !plan.need(at, length, size, "a block")) break;
            block(plan, at, length, textureTable);
            plan.words(at, 2);
            at += length;
        }
    }
    // TEX1 blocks: tag, size, a name, then a ResTIMG and its texels.
    at = plan.be32(0xC);
    for (std::uint32_t t = 0; t < textures && plan.ok(); ++t) {
        if (!plan.need(at, 0x40, size, "a texture") || !plan.is(at, "TEX1")) return plan.ok() ? "has a texture without its TEX1 tag" : plan.failed();
        const std::size_t length = plan.be32(at + 4);
        if (length < 0x40 || !plan.need(at, length, size, "a texture")) break;
        plan.words(at, 2), planResTimg(plan, at + 0x20);
        at += length;
    }
    return plan.apply();
}
} // namespace smg
