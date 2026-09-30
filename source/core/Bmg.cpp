// BMG message files turned native for the game's own readers. The text is walked message by message because the
// game reads it more than one way: characters and a tag's type as 16-bit units, a tag's length and group byte by
// byte (MessageEditorMessageTag), some parameters as bytes, halves or words. Each unit is planned once, and a file
// the plan cannot cover is left untouched with the reason.
#include <smg/Bmg.hpp>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace smg {
namespace {
std::uint32_t be32(const std::uint8_t* at) { return std::uint32_t(at[0]) << 24 | std::uint32_t(at[1]) << 16 | std::uint32_t(at[2]) << 8 | at[3]; }
std::uint32_t be16(const std::uint8_t* at) { return std::uint32_t(at[0]) << 8 | at[1]; }
std::uint32_t native32(const std::uint8_t* at) {
    std::uint32_t value;
    std::memcpy(&value, at, 4);
    return value;
}
void swap16(std::uint8_t* at) { std::swap(at[0], at[1]); }
void swap32(std::uint8_t* at) {
    std::swap(at[0], at[3]);
    std::swap(at[1], at[2]);
}

// What becomes of each two-byte unit of the text.
enum Action : std::uint8_t { Untouched, Swap16, Keep, Swap32, Swap32Rest };

char sProblem[128];
const char* problem(const char* what, unsigned a = 0, unsigned b = 0) {
    std::snprintf(sProblem, sizeof sProblem, what, a, b);
    return sProblem;
}

struct Plan {
    std::vector<std::uint8_t> units;
    const char* failed = nullptr;
    void set(std::size_t unit, Action action) {
        if (units[unit] != Untouched && units[unit] != action) failed = "has messages that share text read two ways";
        units[unit] = action;
    }
};

// A tag's parameters by kind, as the game's tag readers take them (CustomTagProcessor, ReplaceTagProcessor,
// TalkNodeCtrl); `first` is the unit after the type.
const char* planParameters(const std::uint8_t* text, std::size_t first, unsigned group, unsigned type, unsigned units, Plan& plan) {
    if (units == 0) return nullptr;
    if (group == 1 && type == 0 && units == 1) {  // a wait: getParam16
        plan.set(first, Swap16);
    } else if (group == 2 || group == 10) {  // a sound or font name: getParamPtr text
        for (unsigned i = 0; i < units; ++i) plan.set(first + i, Swap16);
    } else if ((group == 5 || (group == 255 && type == 0)) && units == 1) {  // a player name or colour: getParam8
        plan.set(first, Keep);
    } else if ((group == 6 || group == 7) && units == 4) {
        // A slot the game writes natively before it reads (setArgNumber, setArgString), then getParam32(1).
        if (be32(text + first * 2)) return problem("has a number or string tag (group %u, type %u) whose slot isn't empty", group, type);
        plan.set(first, Keep);
        plan.set(first + 1, Keep);
        plan.set(first + 2, Swap32);
        plan.set(first + 3, Swap32Rest);
    } else if (group == 8 && type == 0 && units == 2) {  // a message index: getParam32(0) in TalkNodeCtrl
        plan.set(first, Swap32);
        plan.set(first + 1, Swap32Rest);
    } else {
        return problem("has a tag kind the port has no layout for (group %u, type %u)", group, type);
    }
    return nullptr;
}

const char* planText(const std::uint8_t* text, std::size_t units, std::uint32_t offset, Plan& plan) {
    if (offset % 2) return "has a message at an odd offset";
    for (std::size_t unit = offset / 2;;) {
        if (unit >= units) return "has a message that runs past its text";
        const std::uint32_t code = be16(text + unit * 2);
        if (code != 0x1A) {
            plan.set(unit++, Swap16);
            if (code == 0) return nullptr;
            continue;
        }
        // A tag: 0x1A, its length in bytes from the 0x1A, its group, its type, then parameters.
        if (unit + 3 > units) return "has a tag cut short";
        const unsigned length = text[unit * 2 + 2], group = text[unit * 2 + 3], type = be16(text + unit * 2 + 4);
        if (length < 6 || length % 2 || unit + length / 2 > units) return problem("has a tag of %u bytes (group %u)", length, group);
        plan.set(unit, Swap16);
        plan.set(unit + 1, Keep);  // length and group, read as bytes
        plan.set(unit + 2, Swap16);
        if (const char* failed = planParameters(text, unit + 3, group, type, (length - 6) / 2, plan)) return failed;
        unit += length / 2;
    }
}
} // namespace

const char* nativeBmg(std::uint8_t* data, std::size_t size) {
    if (size < 0x20 || std::memcmp(data, "MESGbmg1", 8)) return "is not a BMG message file";
    // A block count reads small in only one byte order.
    const std::uint32_t nativeBlocks = native32(data + 0xC), blocks = be32(data + 0xC);
    if (nativeBlocks && nativeBlocks < 0x100 && blocks >= 0x100) return nullptr;
    if (!blocks || blocks >= 0x100) return "has no plausible block count";
    if (data[0x10] != 2) return problem("is not UTF-16 (encoding %u)", data[0x10]);

    std::uint8_t *inf = nullptr, *dat = nullptr, *flw = nullptr;
    std::vector<std::uint8_t*> headers;
    std::size_t at = 0x20;
    for (std::uint32_t i = 0; i < blocks; ++i) {
        if (at + 8 > size) return "has a block past its end";
        std::uint8_t* block = data + at;
        const std::uint32_t blockSize = be32(block + 4);
        if (blockSize < 8) return "has a block shorter than its header";
        // Only these blocks' contents are read; FLI1 is kept but never read, and on the disc it runs past the file.
        const bool read = !std::memcmp(block, "INF1", 4) || !std::memcmp(block, "DAT1", 4) || !std::memcmp(block, "FLW1", 4);
        if (read && at + blockSize > size) return "has a block that runs past its end";
        if (!std::memcmp(block, "INF1", 4)) inf = block;
        if (!std::memcmp(block, "DAT1", 4)) dat = block;
        if (!std::memcmp(block, "FLW1", 4)) flw = block;
        headers.push_back(block);
        at += blockSize;
    }
    if (!inf || !dat) return "has no INF1 or DAT1";
    if (be32(inf + 4) < 16) return "has an INF1 shorter than its header";

    const std::uint32_t count = be16(inf + 8), itemSize = be16(inf + 10);
    if (itemSize < 6 || 16 + count * itemSize > be32(inf + 4)) return "has INF1 items that do not fit";
    const std::uint8_t* text = dat + 8;
    const std::size_t textBytes = be32(dat + 4) - 8;
    if (textBytes % 2) return "has an odd-sized DAT1";
    Plan plan;
    plan.units.assign(textBytes / 2, Untouched);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (const char* failed = planText(text, plan.units.size(), be32(inf + 16 + i * itemSize), plan)) return failed;
        if (plan.failed) return plan.failed;
    }

    std::uint32_t nodes = 0, branches = 0;
    if (flw) {
        if (be32(flw + 4) < 16) return "has an FLW1 shorter than its header";
        nodes = be16(flw + 8);
        branches = be16(flw + 10);
        if (16 + nodes * 8 + branches * 3 > be32(flw + 4)) return "has flow tables that do not fit";
        for (std::uint32_t i = 0; i < nodes; ++i) {
            const unsigned type = flw[16 + i * 8];
            if (type < 1 || type > 3) return problem("has a flow node of type %u", type);
        }
    }

    // Everything is planned; only now does anything change.
    swap32(data + 0x8);
    swap32(data + 0xC);
    for (std::uint8_t* block : headers) {
        swap32(block);
        swap32(block + 4);
    }
    swap16(inf + 8);
    swap16(inf + 10);
    swap32(inf + 12);
    for (std::uint32_t i = 0; i < count; ++i) {
        swap32(inf + 16 + i * itemSize);
        swap16(inf + 16 + i * itemSize + 4);
    }
    for (std::size_t unit = 0; unit < plan.units.size(); ++unit) {
        if (plan.units[unit] == Swap16) swap16(dat + 8 + unit * 2);
        else if (plan.units[unit] == Swap32) swap32(dat + 8 + unit * 2);
    }
    if (flw) {
        swap16(flw + 8);
        swap16(flw + 10);
        swap32(flw + 12);
        // Message and branch nodes read two halves after the index; event nodes one word (TalkNodeCtrl, TalkMessageCtrl).
        for (std::uint32_t i = 0; i < nodes; ++i) {
            std::uint8_t* node = flw + 16 + i * 8;
            swap16(node + 2);
            if (node[0] == 3) {
                swap32(node + 4);
            } else {
                swap16(node + 4);
                swap16(node + 6);
            }
        }
        for (std::uint32_t i = 0; i < branches; ++i) swap16(flw + 16 + nodes * 8 + i * 2);
    }
    return nullptr;
}
} // namespace smg
