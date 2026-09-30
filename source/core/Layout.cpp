// nw4r's layout family turned native for nw4r's own readers. Every field is planned once, by the struct its reader
// takes it through, before a byte moves: a file the plan cannot cover is left untouched with the reason.
#include <smg/Layout.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the port turns big-endian data into little-endian");

namespace smg {
namespace {
constexpr std::uint32_t magic(const char (&text)[5]) {
    return std::uint32_t(std::uint8_t(text[0])) << 24 | std::uint32_t(std::uint8_t(text[1])) << 16 |
           std::uint32_t(std::uint8_t(text[2])) << 8 | std::uint8_t(text[3]);
}
std::uint32_t native32(const std::uint8_t* at) {
    std::uint32_t value;
    std::memcpy(&value, at, 4);
    return value;
}

char sProblem[160];

// What becomes of each byte: kept, or part of a half or word that turns.
enum Tag : std::uint8_t { Unread, Keep, Half0, Half1, Word0, Word1, Word2, Word3 };

class Plan {
public:
    Plan(std::uint8_t* data, std::size_t size) : data_(data), size_(size), tags_(size, Unread) {}

    bool ok() const { return failed_ == nullptr; }
    const char* failed() const { return failed_; }
    bool fail(const char* format, ...) {
        if (!failed_) {
            std::va_list args;
            va_start(args, format);
            std::vsnprintf(sProblem, sizeof sProblem, format, args);
            va_end(args);
            failed_ = sProblem;
        }
        return false;
    }

    // Whether [at, at + count) lies before `end` (and inside the file).
    bool fits(std::size_t at, std::size_t count, std::size_t end) const { return end <= size_ && at <= end && count <= end - at; }
    bool need(std::size_t at, std::size_t count, std::size_t end, const char* what) {
        return ok() && (fits(at, count, end) || fail("has %s that runs past its end", what));
    }
    // base + offset, or the file's end when that would pass it (or wrap a 32-bit size_t).
    std::size_t from(std::size_t base, std::uint32_t offset) const { return base <= size_ && offset <= size_ - base ? base + offset : size_; }
    std::uint32_t be8(std::size_t at) const { return data_[at]; }
    std::uint32_t be16(std::size_t at) const { return std::uint32_t(data_[at]) << 8 | data_[at + 1]; }
    std::uint32_t be32(std::size_t at) const { return be16(at) << 16 | be16(at + 2); }
    bool is(std::size_t at, const char (&kind)[5]) const { return std::memcmp(data_ + at, kind, 4) == 0; }

    void keep(std::size_t at, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) set(at + i, Keep);
    }
    // ARM faults on a misaligned float load, so every field turned must sit where its CPU load can take it.
    void half(std::size_t at) {
        if (at % 2) fail("has a half at 0x%zx that ARM cannot load there", at);
        set(at, Half0), set(at + 1, Half1);
    }
    void halves(std::size_t at, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) half(at + 2 * i);
    }
    void word(std::size_t at) {
        if (at % 4) fail("has a word at 0x%zx that ARM cannot load there", at);
        set(at, Word0), set(at + 1, Word1), set(at + 2, Word2), set(at + 3, Word3);
    }
    void words(std::size_t at, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) word(at + 4 * i);
    }
    // A NUL-terminated name, read as bytes.
    bool name(std::size_t at, std::size_t end, const char* what) {
        if (!need(at, 1, end, what)) return false;
        const void* nul = std::memchr(data_ + at, 0, end - at);
        if (!nul) return fail("has %s with no end", what);
        keep(at, static_cast<const std::uint8_t*>(nul) - (data_ + at) + 1);
        return true;
    }

    const char* apply() {
        if (failed_) return failed_;
        for (std::size_t i = 0; i < size_; ++i) {
            if (tags_[i] == Half0) std::swap(data_[i], data_[i + 1]);
            if (tags_[i] == Word0) {
                std::swap(data_[i], data_[i + 3]);
                std::swap(data_[i + 1], data_[i + 2]);
            }
        }
        return nullptr;
    }

private:
    void set(std::size_t at, Tag tag) {
        if (at >= size_) return void(fail("has a field past its end"));
        if (tags_[at] != Unread && tags_[at] != tag) fail("has bytes at 0x%zx that two readers take different ways", at);
        tags_[at] = tag;
    }

    std::uint8_t* data_;
    std::size_t size_;
    std::vector<std::uint8_t> tags_;
    const char* failed_ = nullptr;
};

// nw4r's BinaryFileHeader (lyt::res and ut alike): a signature compared as a word, then the byte order mark.
struct File {
    std::size_t end = 0, first = 0;
    unsigned blocks = 0;
};
bool fileHeader(Plan& plan, std::size_t size, const char (&signature)[5], const char* format, File& file) {
    if (size < 16) return plan.fail("is shorter than a %s header", format);
    if (!plan.is(0, signature)) return plan.fail("isn't a %s file", format);
    if (plan.be16(4) != 0xFEFF) return plan.fail("has no big-endian byte order mark");
    plan.word(0), plan.half(4), plan.half(6), plan.word(8), plan.half(12), plan.half(14);
    file.end = plan.be32(8);
    file.first = plan.be16(12);
    file.blocks = plan.be16(14);
    if (file.end > size) return plan.fail("says it is %zu bytes, but is %zu", file.end, size);
    return true;
}

// Walks the blocks after the header: `block(at, end)` plans one whose kind and size are already planned.
template <typename Block>
const char* walk(Plan& plan, const File& file, Block block) {
    std::size_t at = file.first;
    for (unsigned i = 0; plan.ok() && i < file.blocks; ++i) {
        if (!plan.need(at, 8, file.end, "a block header")) break;
        const std::size_t size = plan.be32(at + 4);
        if (size < 8 || !plan.fits(at, size, file.end)) return plan.fail("has a block whose size runs past the file"), plan.failed();
        plan.word(at), plan.word(at + 4);
        block(at, at + size);
        at += size;
    }
    return plan.failed();
}

// ---- BRLYT (nw4r::lyt::res, lyt_layout.cpp, lyt_pane.cpp, lyt_picture.cpp, lyt_textBox.cpp, lyt_window.cpp,
// lyt_group.cpp, lyt_material.cpp)

struct Lists {
    unsigned textures = 0, fonts = 0, materials = 0;
    bool haveTextures = false, haveFonts = false, haveMaterials = false;
};

bool nameList(Plan& plan, std::size_t at, std::size_t end, unsigned& count, bool& have) {
    if (!plan.need(at + 8, 4, end, "a name list")) return false;
    count = plan.be16(at + 8);
    have = true;
    plan.half(at + 8), plan.keep(at + 10, 2);
    const std::size_t list = at + 12;
    if (!plan.need(list, 8 * std::size_t(count), end, "a name list")) return false;
    for (unsigned i = 0; i < count && plan.ok(); ++i) {
        plan.word(list + 8 * i), plan.keep(list + 8 * i + 4, 4);  // res::Texture / res::Font: offset, type
        plan.name(plan.from(list, plan.be32(list + 8 * i)), end, "a texture or font name");
    }
    return plan.ok();
}

bool material(Plan& plan, std::size_t m, std::size_t end, const Lists& lists) {
    if (!plan.need(m, 64, end, "a material")) return false;
    plan.keep(m, 20), plan.halves(m + 20, 12), plan.keep(m + 44, 16), plan.word(m + 60);  // name, GXColorS10 x3, GXColor x4
    const std::uint32_t bits = plan.be32(m + 60);
    if (bits & 0xF4000000u) return plan.fail("has a material with resource bits the port has no layout for (0x%08x)", unsigned(bits));
    const auto field = [bits](int shift, int length) { return (bits >> shift) & ((1u << length) - 1); };
    std::size_t at = m + 64;
    const auto run = [&](unsigned count, std::size_t size, auto each) {
        if (!plan.need(at, count * size, end, "material resources")) return;
        for (unsigned i = 0; i < count; ++i) each(at + i * size);
        at += count * size;
    };
    run(field(0, 4), 4, [&](std::size_t t) {  // res::TexMap
        plan.half(t), plan.keep(t + 2, 2);
        if (!lists.haveTextures || plan.be16(t) >= lists.textures) plan.fail("has a material whose texture isn't in the texture list");
    });
    run(field(4, 4), 20, [&](std::size_t t) { plan.words(t, 5); });  // TexSRT
    run(field(8, 4), 4, [&](std::size_t t) { plan.keep(t, 4); });    // TexCoordGen
    run(field(25, 1), 4, [&](std::size_t t) { plan.keep(t, 4); });   // ChanCtrl
    run(field(27, 1), 4, [&](std::size_t t) { plan.keep(t, 4); });   // ut::Color, copied as bytes
    run(field(12, 1), 4, [&](std::size_t t) { plan.keep(t, 4); });   // TevSwapMode x4
    run(field(13, 2), 20, [&](std::size_t t) { plan.words(t, 5); }); // indirect TexSRT
    run(field(15, 3), 4, [&](std::size_t t) { plan.keep(t, 4); });   // IndirectStage
    run(field(18, 5), 16, [&](std::size_t t) { plan.keep(t, 16); }); // TevStage
    run(field(23, 1), 4, [&](std::size_t t) { plan.keep(t, 4); });   // AlphaCompare
    run(field(24, 1), 4, [&](std::size_t t) { plan.keep(t, 4); });   // BlendMode
    return plan.ok();
}

bool materialIndex(Plan& plan, std::size_t at, const Lists& lists) {
    plan.half(at);
    return (lists.haveMaterials && plan.be16(at) < lists.materials) || plan.fail("has a pane whose material isn't in the material list");
}

// res::Pane: flag, base position, alpha, padding, name, user data, then translate, rotate, scale and size.
bool pane(Plan& plan, std::size_t at, std::size_t end) {
    if (!plan.need(at, 0x4C, end, "a pane")) return false;
    plan.keep(at + 8, 28), plan.words(at + 36, 10);
    return true;
}

// Four vertex colours (u32s nw4r turns into ut::Color by value), a material, then texture coordinate quads.
bool vertexColours(Plan& plan, std::size_t at, std::size_t end, const Lists& lists) {
    if (!plan.need(at, 20, end, "a picture or window content")) return false;
    plan.words(at, 4), plan.keep(at + 18, 2);
    materialIndex(plan, at + 16, lists);
    const unsigned coordinates = plan.be8(at + 18);
    if (!plan.need(at + 20, 32 * std::size_t(coordinates), end, "texture coordinates")) return false;
    plan.words(at + 20, 8 * coordinates);
    return plan.ok();
}

bool textBox(Plan& plan, std::size_t at, std::size_t end, const Lists& lists) {
    if (!plan.need(at, 0x74, end, "a text box")) return false;
    plan.halves(at + 0x4C, 2), plan.keep(at + 0x54, 4), plan.word(at + 0x58), plan.words(at + 0x5C, 2), plan.words(at + 0x64, 4);
    materialIndex(plan, at + 0x50, lists);
    plan.half(at + 0x52);
    if (!lists.haveFonts || plan.be16(at + 0x52) >= lists.fonts) return plan.fail("has a text box whose font isn't in the font list");
    const std::size_t bytes = plan.be16(at + 0x4E), offset = plan.be32(at + 0x58);
    if (bytes % 2) return plan.fail("has text of an odd number of bytes");
    if (bytes && (offset < 0x74 || !plan.need(plan.from(at, offset), bytes, end, "text"))) return plan.fail("has text outside its text box");
    plan.halves(plan.from(at, offset), bytes / 2);  // UTF-16, read through wchar_t
    return plan.ok();
}

bool window(Plan& plan, std::size_t at, std::size_t end, const Lists& lists) {
    if (!plan.need(at, 0x68, end, "a window")) return false;
    plan.words(at + 0x4C, 4), plan.keep(at + 0x5C, 4), plan.words(at + 0x60, 2);  // inflation, frame count, offsets
    if (!vertexColours(plan, plan.from(at, plan.be32(at + 0x60)), end, lists)) return false;
    const unsigned frames = plan.be8(at + 0x5C);
    const std::size_t table = plan.from(at, plan.be32(at + 0x64));
    if (!plan.need(table, 4 * std::size_t(frames), end, "a window's frame table")) return false;
    plan.words(table, frames);
    for (unsigned i = 0; i < frames && plan.ok(); ++i) {
        const std::size_t frame = plan.from(at, plan.be32(table + 4 * i));
        if (!plan.need(frame, 4, end, "a window frame")) break;
        materialIndex(plan, frame, lists);
        plan.keep(frame + 2, 2);
    }
    return plan.ok();
}

bool group(Plan& plan, std::size_t at, std::size_t end) {
    if (!plan.need(at, 28, end, "a group")) return false;
    plan.keep(at + 8, 16), plan.half(at + 24), plan.keep(at + 26, 2);
    const std::size_t panes = plan.be16(at + 24);
    if (!plan.need(at + 28, 16 * panes, end, "a group's pane names")) return false;
    plan.keep(at + 28, 16 * panes);
    return true;
}

// ---- BRLAN (lyt_animation.cpp)

bool animationBlock(Plan& plan, std::size_t at, std::size_t end) {
    if (!plan.need(at, 20, end, "an animation block")) return false;
    plan.half(at + 8), plan.keep(at + 10, 2), plan.halves(at + 12, 2), plan.word(at + 16);
    const unsigned files = plan.be16(at + 12), contents = plan.be16(at + 14);
    const std::size_t names = at + 20;  // GetStrTableStr: offsets from the table's start
    if (!plan.need(names, 4 * std::size_t(files), end, "a texture file table")) return false;
    plan.words(names, files);
    for (unsigned i = 0; i < files && plan.ok(); ++i) plan.name(plan.from(names, plan.be32(names + 4 * i)), end, "a texture file name");
    const std::size_t table = plan.from(at, plan.be32(at + 16));
    if (!plan.need(table, 4 * std::size_t(contents), end, "an animation content table")) return false;
    plan.words(table, contents);
    for (unsigned c = 0; c < contents && plan.ok(); ++c) {
        const std::size_t content = plan.from(at, plan.be32(table + 4 * c));  // res::AnimationContent
        if (!plan.need(content, 24, end, "an animation content")) return false;
        plan.keep(content, 24);
        const unsigned infos = plan.be8(content + 20);
        if (!plan.need(content + 24, 4 * std::size_t(infos), end, "an animation content")) return false;
        plan.words(content + 24, infos);
        for (unsigned i = 0; i < infos && plan.ok(); ++i) {
            const std::size_t info = plan.from(content, plan.be32(content + 24 + 4 * i));  // res::AnimationInfo
            if (!plan.need(info, 8, end, "an animation")) return false;
            plan.word(info), plan.keep(info + 4, 4);
            const std::uint32_t kind = plan.be32(info);
            const bool step = kind == magic("RLVI") || kind == magic("RLTP");
            if (!step && kind != magic("RLPA") && kind != magic("RLVC") && kind != magic("RLMC") && kind != magic("RLTS") &&
                kind != magic("RLIM")) {
                return plan.fail("has an animation kind the port has no layout for (0x%08x)", unsigned(kind));
            }
            const unsigned targets = plan.be8(info + 4);
            if (!plan.need(info + 8, 4 * std::size_t(targets), end, "an animation")) return false;
            plan.words(info + 8, targets);
            for (unsigned t = 0; t < targets && plan.ok(); ++t) {
                const std::size_t target = plan.from(info, plan.be32(info + 8 + 4 * t));  // res::AnimationTarget
                if (!plan.need(target, 12, end, "an animation target")) return false;
                plan.keep(target, 4), plan.half(target + 4), plan.keep(target + 6, 2), plan.word(target + 8);
                // nw4r picks the key layout by the animation's kind; the file's curve type must agree.
                if (plan.be8(target + 2) != (step ? 1u : 2u)) return plan.fail("has a curve its reader would take as another kind");
                const std::size_t keys = plan.from(target, plan.be32(target + 8)), count = plan.be16(target + 4);
                if (!plan.need(keys, count * (step ? 8 : 12), end, "animation keys")) return false;
                for (std::size_t k = 0; k < count; ++k) {
                    if (step) plan.word(keys + 8 * k), plan.halves(keys + 8 * k + 4, 2);  // res::StepKey
                    else plan.words(keys + 12 * k, 3);                                    // res::HermiteKey
                }
            }
        }
    }
    return plan.ok();
}

bool tagBlock(Plan& plan, std::size_t at, std::size_t end) {
    if (!plan.need(at, 28, end, "an animation tag")) return false;
    plan.halves(at + 8, 2), plan.words(at + 12, 2), plan.halves(at + 20, 2), plan.keep(at + 24, 4);
    plan.name(plan.from(at, plan.be32(at + 12)), end, "an animation tag's name");
    const std::size_t groups = plan.from(at, plan.be32(at + 16)), count = plan.be16(at + 10);
    if (count && plan.need(groups, 20 * count, end, "an animation tag's groups")) plan.keep(groups, 20 * count);  // name, flag, padding
    return plan.ok();
}

// ---- BRFNT (ut_ResFont.cpp, ut_ResFontBase.cpp, fontResourceFormat.h)

bool fontOffset(Plan& plan, std::uint32_t offset, std::size_t size) {
    return offset < size || plan.fail("has a font offset past the file (0x%08x)", unsigned(offset));
}
} // namespace

const char* nativeBrlyt(std::uint8_t* data, std::size_t size) {
    if (size >= 4 && native32(data) == magic("RLYT")) return nullptr;
    Plan plan(data, size);
    File file;
    if (!fileHeader(plan, size, "RLYT", "BRLYT", file)) return plan.failed();
    Lists lists;
    const char* why = walk(plan, file, [&](std::size_t at, std::size_t end) {
        if (plan.is(at, "lyt1")) {
            if (plan.need(at, 20, end, "a layout block")) plan.keep(at + 8, 4), plan.words(at + 12, 2);
        } else if (plan.is(at, "txl1")) {
            nameList(plan, at, end, lists.textures, lists.haveTextures);
        } else if (plan.is(at, "fnl1")) {
            nameList(plan, at, end, lists.fonts, lists.haveFonts);
        } else if (plan.is(at, "mat1")) {
            if (!plan.need(at, 12, end, "a material list")) return;
            lists.materials = plan.be16(at + 8);
            lists.haveMaterials = true;
            plan.half(at + 8), plan.keep(at + 10, 2);
            if (!plan.need(at + 12, 4 * std::size_t(lists.materials), end, "a material list")) return;
            plan.words(at + 12, lists.materials);
            for (unsigned i = 0; i < lists.materials && plan.ok(); ++i) material(plan, plan.from(at, plan.be32(at + 12 + 4 * i)), end, lists);
        } else if (plan.is(at, "pan1") || plan.is(at, "bnd1")) {
            pane(plan, at, end);
        } else if (plan.is(at, "pic1")) {
            if (pane(plan, at, end)) vertexColours(plan, at + 0x4C, end, lists);
        } else if (plan.is(at, "txt1")) {
            if (pane(plan, at, end)) textBox(plan, at, end, lists);
        } else if (plan.is(at, "wnd1")) {
            if (pane(plan, at, end)) window(plan, at, end, lists);
        } else if (plan.is(at, "grp1")) {
            group(plan, at, end);
        } else if (!plan.is(at, "pas1") && !plan.is(at, "pae1") && !plan.is(at, "grs1") && !plan.is(at, "gre1")) {
            plan.fail("has a block the port has no layout for (%.4s)", reinterpret_cast<const char*>(data + at));
        }
    });
    return why ? why : plan.apply();
}

const char* nativeBrlan(std::uint8_t* data, std::size_t size) {
    if (size >= 4 && native32(data) == magic("RLAN")) return nullptr;
    Plan plan(data, size);
    File file;
    if (!fileHeader(plan, size, "RLAN", "BRLAN", file)) return plan.failed();
    const char* why = walk(plan, file, [&](std::size_t at, std::size_t end) {
        if (plan.is(at, "pat1")) tagBlock(plan, at, end);
        else if (plan.is(at, "pai1")) animationBlock(plan, at, end);
        else plan.fail("has a block the port has no layout for (%.4s)", reinterpret_cast<const char*>(data + at));
    });
    return why ? why : plan.apply();
}

// TPLPalette, TPLDescriptor, TPLHeader and TPLClutHeader (revolution/tpl.h, with its enums as the Wii's 4 bytes).
const char* nativeTpl(std::uint8_t* data, std::size_t size) {
    constexpr std::uint32_t version = 2142000;
    if (size >= 4 && native32(data) == version) return nullptr;
    Plan plan(data, size);
    if (size < 12 || plan.be32(0) != version) return plan.fail("isn't a texture palette (TPL)"), plan.failed();
    plan.words(0, 3);
    const std::size_t count = plan.be32(4), descriptors = plan.be32(8);
    if (!plan.need(descriptors, 8 * count, size, "a texture descriptor table")) return plan.failed();
    plan.words(descriptors, 2 * count);
    for (std::size_t i = 0; i < count && plan.ok(); ++i) {
        if (const std::size_t header = plan.be32(descriptors + 8 * i)) {
            if (!plan.need(header, 36, size, "a texture header")) break;
            plan.halves(header, 2), plan.words(header + 4, 7), plan.keep(header + 32, 4);
            if (plan.be8(header + 35)) plan.fail("has a texture already bound to its memory");
            if (plan.be32(header + 8) >= size) plan.fail("has texels past the file");
        }
        if (const std::size_t clut = plan.be32(descriptors + 8 * i + 4)) {
            if (!plan.need(clut, 12, size, "a palette header")) break;
            plan.half(clut), plan.keep(clut + 2, 2), plan.words(clut + 4, 2);
            if (plan.be8(clut + 2)) plan.fail("has a palette already bound to its memory");
            if (plan.be32(clut + 8) >= size) plan.fail("has palette entries past the file");
        }
    }
    return plan.apply();
}

const char* nativeBrfnt(std::uint8_t* data, std::size_t size) {
    if (size >= 4 && (native32(data) == magic("RFNT") || native32(data) == magic("RFNU"))) return nullptr;
    Plan plan(data, size);
    File file;
    if (!fileHeader(plan, size, "RFNT", "BRFNT", file)) return plan.failed();
    const char* why = walk(plan, file, [&](std::size_t at, std::size_t end) {
        if (plan.is(at, "FINF")) {  // FontInformation
            if (!plan.need(at + 8, 24, end, "font information")) return;
            plan.keep(at + 8, 2), plan.half(at + 10), plan.keep(at + 12, 4), plan.words(at + 16, 3), plan.keep(at + 28, 4);
            for (int i = 0; i < 3; ++i) fontOffset(plan, plan.be32(at + 16 + 4 * i), file.end);
        } else if (plan.is(at, "TGLP")) {  // FontTextureGlyph; the sheets stay as the GPU reads them
            if (!plan.need(at + 8, 24, end, "a glyph sheet header")) return;
            plan.keep(at + 8, 4), plan.word(at + 12), plan.halves(at + 16, 6), plan.word(at + 28);
            const std::size_t sheets = std::size_t(plan.be32(at + 12)) * plan.be16(at + 16);
            if (!plan.fits(plan.be32(at + 28), sheets, file.end)) plan.fail("has glyph sheets past the file");
        } else if (plan.is(at, "CWDH")) {  // FontWidth
            if (!plan.need(at + 8, 8, end, "a width table")) return;
            plan.halves(at + 8, 2), plan.word(at + 12);
            fontOffset(plan, plan.be32(at + 12), file.end);
            const unsigned first = plan.be16(at + 8), last = plan.be16(at + 10);
            if (last < first) return void(plan.fail("has a width table that ends before it begins"));
            if (plan.need(at + 16, 3 * std::size_t(last - first + 1), end, "a width table")) plan.keep(at + 16, 3 * (last - first + 1));
        } else if (plan.is(at, "CMAP")) {  // FontCodeMap
            if (!plan.need(at + 8, 14, end, "a code map")) return;
            plan.halves(at + 8, 4), plan.word(at + 16);
            fontOffset(plan, plan.be32(at + 16), file.end);
            const unsigned first = plan.be16(at + 8), last = plan.be16(at + 10), method = plan.be16(at + 12);
            std::size_t units = 0;
            if (method == 0) units = 1;                                   // direct: the first glyph
            else if (method == 1 && last >= first) units = last - first + 1;  // table: a glyph per code
            else if (method == 2) units = 1 + 2 * std::size_t(plan.be16(at + 20));  // scan: count, then (code, glyph) pairs
            else return void(plan.fail("has a code map the port has no layout for (method %u)", method));
            if (plan.need(at + 20, 2 * units, end, "a code map")) plan.halves(at + 20, units);
        } else {
            plan.fail("has a block the port has no layout for (%.4s)", reinterpret_cast<const char*>(data + at));
        }
    });
    return why ? why : plan.apply();
}
} // namespace smg
