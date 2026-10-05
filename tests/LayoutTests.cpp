#include <nw4r/lyt/resources.h>
#include <nw4r/ut/Color.h>
#include <smg/Layout.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

using Bytes = std::vector<std::uint8_t>;
void put8(Bytes& out, unsigned value) { out.push_back(std::uint8_t(value)); }
void put16(Bytes& out, unsigned value) { out.insert(out.end(), {std::uint8_t(value >> 8), std::uint8_t(value)}); }
void put32(Bytes& out, std::uint32_t value) { put16(out, value >> 16); put16(out, value & 0xFFFF); }
void putf(Bytes& out, float value) { std::uint32_t bits; std::memcpy(&bits, &value, 4); put32(out, bits); }
void putName(Bytes& out, const char* name, std::size_t size) {
    const std::size_t at = out.size();
    out.resize(at + size);
    std::memcpy(out.data() + at, name, std::strlen(name));
}
void align(Bytes& out) { while (out.size() % 4) out.push_back(0); }
void set32(Bytes& out, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[at + i] = std::uint8_t(value >> (24 - 8 * i));
}
std::uint16_t native16(const std::uint8_t* at) { std::uint16_t v; std::memcpy(&v, at, 2); return v; }
std::uint32_t native32(const std::uint8_t* at) { std::uint32_t v; std::memcpy(&v, at, 4); return v; }
float nativef(const std::uint8_t* at) { float v; std::memcpy(&v, at, 4); return v; }

// A block: kind, size, then what `body` writes.
template <typename Body>
void block(Bytes& out, const char* kind, Body body) {
    const std::size_t at = out.size();
    putName(out, kind, 4);
    put32(out, 0);
    body(at);
    align(out);
    set32(out, at + 4, std::uint32_t(out.size() - at));
}
Bytes file(const char* signature, unsigned blocks) {
    Bytes b;
    putName(b, signature, 4);
    put16(b, 0xFEFF), put16(b, signature[1] == 'F' ? 0x104 : 0x0A), put32(b, 0), put16(b, 16), put16(b, blocks);
    return b;
}
void finish(Bytes& b) { set32(b, 8, std::uint32_t(b.size())); }

void pane(Bytes& b, const char* name, float x) {
    put8(b, 1), put8(b, 4), put8(b, 200), put8(b, 0);
    putName(b, name, 16), putName(b, "user", 8);
    for (float f : {x, -2.0f, 0.0f, 0.0f, 0.0f, 90.0f, 1.0f, 1.0f, 608.0f, 456.0f}) putf(b, f);
}

struct Layout {
    Bytes bytes;
    std::size_t lyt = 0, mat = 0, material0 = 0, material1 = 0, pic = 0, txt = 0, wnd = 0, grp = 0;
};

// Every kind of block the disc uses, and a material with every resource the disc's flag words turn on.
Layout layout(std::uint32_t material0Bits = 0x0b881111, std::uint32_t textOffset = 0x74) {
    Layout l;
    Bytes& b = l.bytes = file("RLYT", 15);
    block(b, "lyt1", [&](std::size_t at) { l.lyt = at; put8(b, 1), b.resize(b.size() + 3), putf(b, 608), putf(b, 456); });
    block(b, "txl1", [&](std::size_t) { put16(b, 1), put16(b, 0), put32(b, 8), put32(b, 0), putName(b, "a.tpl", 6); });
    block(b, "fnl1", [&](std::size_t) { put16(b, 1), put16(b, 0), put32(b, 8), put32(b, 0), putName(b, "f.brfnt", 8); });
    block(b, "mat1", [&](std::size_t at) {
        l.mat = at;
        put16(b, 2), put16(b, 0), put32(b, 0), put32(b, 0);
        l.material0 = b.size();
        set32(b, at + 12, std::uint32_t(l.material0 - at));
        putName(b, "M0", 20);
        for (int i = 0; i < 3; ++i) put16(b, 1), put16(b, 0xFFFE), put16(b, 300), put16(b, 0xFC00);  // s16: 1, -2, 300, -1024
        for (int i = 0; i < 4; ++i) put8(b, 1), put8(b, 2), put8(b, 3), put8(b, 4);
        put32(b, material0Bits);
        put16(b, 0), put8(b, 0x01), put8(b, 0x02);                                   // TexMap
        for (float f : {0.25f, -0.5f, 45.0f, 2.0f, 3.0f}) putf(b, f);               // TexSRT
        put8(b, 1), put8(b, 4), put8(b, 30), put8(b, 0);                             // TexCoordGen
        put8(b, 1), put8(b, 0), put8(b, 0), put8(b, 0);                              // ChanCtrl
        put8(b, 0x11), put8(b, 0x22), put8(b, 0x33), put8(b, 0x44);                  // material color
        put8(b, 0xE4), put8(b, 0xE4), put8(b, 0xE4), put8(b, 0xE4);                  // TevSwapMode x4
        for (int i = 0; i < 32; ++i) put8(b, i);                                     // TevStage x2
        put8(b, 7), put8(b, 0), put8(b, 128), put8(b, 0);                            // AlphaCompare
        put8(b, 1), put8(b, 4), put8(b, 5), put8(b, 3);                              // BlendMode
        l.material1 = b.size();
        set32(b, at + 16, std::uint32_t(l.material1 - at));
        putName(b, "M1", 20);
        b.resize(b.size() + 40);
        put32(b, 0x0000A000);                                                        // one indirect SRT and stage
        for (float f : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f}) putf(b, f);
        put8(b, 0), put8(b, 1), put8(b, 2), put8(b, 3);
    });
    block(b, "pan1", [&](std::size_t) { pane(b, "RootPane", 1.5f); });
    block(b, "pas1", [&](std::size_t) {});
    block(b, "pic1", [&](std::size_t at) {
        l.pic = at;
        pane(b, "Pic", 2.5f);
        for (std::uint32_t c : {0x11223344u, 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu}) put32(b, c);
        put16(b, 0), put8(b, 1), put8(b, 0);
        for (float f : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f}) putf(b, f);
    });
    block(b, "txt1", [&](std::size_t at) {
        l.txt = at;
        pane(b, "Text", 3.5f);
        put16(b, 8), put16(b, 6), put16(b, 1), put16(b, 0), put8(b, 4), put8(b, 1), put16(b, 0);
        put32(b, textOffset), put32(b, 0x000000FF), put32(b, 0xFFFFFFFF);
        for (float f : {24.0f, 26.0f, 1.0f, 2.0f}) putf(b, f);
        put16(b, 'H'), put16(b, 0x3042), put16(b, 0);  // "Hあ"
    });
    block(b, "wnd1", [&](std::size_t at) {
        l.wnd = at;
        pane(b, "Win", 4.5f);
        for (float f : {1.0f, 2.0f, 3.0f, 4.0f}) putf(b, f);
        put8(b, 1), put8(b, 0), put16(b, 0), put32(b, 0x68), put32(b, 0x68 + 20);
        for (std::uint32_t c : {0x01020304u, 0x05060708u, 0x090A0B0Cu, 0x0D0E0F10u}) put32(b, c);
        put16(b, 1), put8(b, 0), put8(b, 0);
        put32(b, 0x68 + 24);
        put16(b, 0), put8(b, 3), put8(b, 0);
    });
    block(b, "bnd1", [&](std::size_t) { pane(b, "Bound", 5.5f); });
    block(b, "pae1", [&](std::size_t) {});
    block(b, "grp1", [&](std::size_t) { putName(b, "RootGroup", 16), put16(b, 0), put16(b, 0); });
    block(b, "grs1", [&](std::size_t) {});
    block(b, "grp1", [&](std::size_t at) { l.grp = at; putName(b, "G", 16), put16(b, 1), put16(b, 0), putName(b, "Pic", 16); });
    block(b, "gre1", [&](std::size_t) {});
    finish(b);
    return l;
}

void layouts() {
    using namespace nw4r::lyt;
    Layout l = layout();
    const Bytes original = l.bytes;
    std::uint8_t* d = l.bytes.data();
    check(!smg::nativeBrlyt(d, l.bytes.size()), "a layout with every block kind on the disc turns native");
    const auto* header = reinterpret_cast<const res::BinaryFileHeader*>(d);
    check(native32(d) == 0x524C5954 && header->byteOrder == 0xFEFF && header->version == 0x0A && header->dataBlocks == 15 &&
          header->fileSize == l.bytes.size(), "the file header reads as nw4r's TestFileHeader and TestFileVersion take it");
    const auto* lyt = reinterpret_cast<const res::Layout*>(d + l.lyt);
    check(native32(d + l.lyt) == 0x6C797431 && lyt->blockHeader.size == 20 && lyt->originType == 1 && lyt->layoutSize.width == 608 &&
          lyt->layoutSize.height == 456, "lyt1 reads through res::Layout");

    const auto* pic = reinterpret_cast<const res::Picture*>(d + l.pic);
    check(std::strcmp(pic->name, "Pic") == 0 && pic->flag == 1 && pic->basePosition == 4 && pic->alpha == 200 && pic->translate.x == 2.5f &&
          pic->translate.y == -2.0f && pic->rotate.z == 90.0f && pic->scale.y == 1.0f && pic->size.width == 608, "a pane reads through res::Pane");
    const nw4r::ut::Color vertex(pic->vtxCols[0]);
    check(vertex.r == 0x11 && vertex.g == 0x22 && vertex.b == 0x33 && vertex.a == 0x44, "a vertex color becomes the ut::Color the Wii built from it");
    const auto* coords = reinterpret_cast<const float*>(d + l.pic + sizeof(res::Picture));
    check(pic->materialIdx == 0 && pic->texCoordNum == 1 && coords[2] == 1.0f && coords[7] == 1.0f, "a picture's material and texture coordinates read natively");

    const auto* txt = reinterpret_cast<const res::TextBox*>(d + l.txt);
    const auto* text = reinterpret_cast<const std::uint16_t*>(d + l.txt + txt->textStrOffset);
    check(txt->textBufBytes == 8 && txt->textStrBytes == 6 && txt->materialIdx == 1 && txt->fontIdx == 0 && txt->textPosition == 4 &&
          txt->textAlignment == 1 && txt->fontSize.height == 26 && txt->lineSpace == 2, "a text box reads through res::TextBox");
    check(text[0] == 'H' && text[1] == 0x3042 && text[2] == 0, "its text reads as native UTF-16 units");
    const nw4r::ut::Color black(txt->textCols[0]), white(txt->textCols[1]);
    check(black.r == 0 && black.a == 0xFF && white.g == 0xFF, "text colors become ut::Colors by value");

    const auto* wnd = reinterpret_cast<const res::Window*>(d + l.wnd);
    const auto* content = reinterpret_cast<const res::WindowContent*>(d + l.wnd + wnd->contentOffset);
    const auto* frames = reinterpret_cast<const std::uint32_t*>(d + l.wnd + wnd->frameOffsetTableOffset);
    const auto* frame = reinterpret_cast<const res::WindowFrame*>(d + l.wnd + frames[0]);
    check(wnd->inflation.r == 2.0f && wnd->frameNum == 1 && content->materialIdx == 1 && nw4r::ut::Color(content->vtxCols[3]).r == 0x0D &&
          frame->materialIdx == 0 && frame->textureFlip == 3, "a window, its content and its frame read through nw4r's structs");

    const auto* mat = reinterpret_cast<const res::MaterialList*>(d + l.mat);
    const auto* offsets = reinterpret_cast<const std::uint32_t*>(d + l.mat + sizeof(res::MaterialList));
    const auto* m0 = reinterpret_cast<const res::Material*>(d + l.mat + offsets[0]);
    check(mat->materialNum == 2 && std::strcmp(m0->name, "M0") == 0 && m0->tevCols[2].g == -2 && m0->tevCols[0].b == 300 &&
          m0->tevCols[1].a == -1024 && m0->tevKCols[3].b == 3, "a material's name and TEV colors read through res::Material");
    check(m0->resNum.GetTexMapNum() == 1 && m0->resNum.GetTevStageNum() == 2 && m0->resNum.HasBlendMode() && m0->resNum.GetMatColNum() == 1,
          "its resource bits read as nw4r's MaterialResourceNum takes them");
    const std::uint8_t* r = d + l.material0 + sizeof(res::Material);
    const auto* texMap = reinterpret_cast<const res::TexMap*>(r);
    const auto* srt = reinterpret_cast<const TexSRT*>(r + 4);
    check(texMap->texIdx == 0 && texMap->GetWarpModeS() == GX_REPEAT && texMap->GetWarpModeT() == GX_MIRROR && srt->translate.y == -0.5f &&
          srt->rotate == 45.0f && srt->scale.y == 3.0f, "a texture map and texture SRT read natively");
    const auto* color = reinterpret_cast<const nw4r::ut::Color*>(r + 32);
    check(color->r == 0x11 && color->a == 0x44 && r[36] == 0xE4 && r[40] == 0 && r[71] == 31 && r[72] == 7 && r[79] == 3,
          "byte-wise resources keep their bytes");
    const auto* m1 = reinterpret_cast<const res::Material*>(d + l.mat + offsets[1]);
    const auto* indirect = reinterpret_cast<const TexSRT*>(d + l.material1 + sizeof(res::Material));
    check(m1->resNum.GetIndTexSRTNum() == 1 && m1->resNum.GetIndTexStageNum() == 1 && indirect->scale.x == 4.0f &&
          d[l.material1 + sizeof(res::Material) + 23] == 3, "indirect texture SRTs turn and indirect stages keep their bytes");

    const auto* group = reinterpret_cast<const res::Group*>(d + l.grp);
    check(std::strcmp(group->name, "G") == 0 && group->paneNum == 1 && std::strcmp(reinterpret_cast<const char*>(group + 1), "Pic") == 0,
          "a group and its pane names read through res::Group");

    const Bytes once = l.bytes;
    check(!smg::nativeBrlyt(d, l.bytes.size()) && l.bytes == once && l.bytes != original, "a native layout is left as it is");
}

void refusesLayouts() {
    Layout l = layout(0x10000000);
    const Bytes before = l.bytes;
    const char* why = smg::nativeBrlyt(l.bytes.data(), l.bytes.size());
    check(why && std::strstr(why, "resource bits") && l.bytes == before, "a material bit the port has no layout for is named and nothing changes");
    l = layout(0x0b881111, 0x40);
    why = smg::nativeBrlyt(l.bytes.data(), l.bytes.size());
    check(why && std::strstr(why, "outside its text box"), "text inside the text box's own header is refused");
    l = layout();
    std::memcpy(l.bytes.data() + l.grp, "usd1", 4);
    why = smg::nativeBrlyt(l.bytes.data(), l.bytes.size());
    check(why && std::strstr(why, "usd1"), "an unknown block kind is named");
    l = layout();
    set32(l.bytes, 8, std::uint32_t(l.bytes.size() + 4));
    check(smg::nativeBrlyt(l.bytes.data(), l.bytes.size()) != nullptr, "a file that says it is longer than it is is refused");
}

// A tag with one group, then two pane animations (SRT, visibility) and a material's texture pattern.
Bytes animation(unsigned visibilityCurve = 1) {
    Bytes b = file("RLAN", 2);
    block(b, "pat1", [&](std::size_t) {
        put16(b, 0), put16(b, 1), put32(b, 28), put32(b, 36), put16(b, 0), put16(b, 60), put8(b, 1), b.resize(b.size() + 3);
        putName(b, "Tag", 8), putName(b, "G", 16), put8(b, 0), b.resize(b.size() + 3);
    });
    block(b, "pai1", [&](std::size_t at) {
        put16(b, 60), put8(b, 1), put8(b, 0), put16(b, 1), put16(b, 2), put32(b, 0);
        put32(b, 4), putName(b, "b.tpl", 8);
        set32(b, at + 16, std::uint32_t(b.size() - at));
        const std::size_t table = b.size();
        put32(b, 0), put32(b, 0);
        set32(b, table, std::uint32_t(b.size() - at));
        const std::size_t pic = b.size();
        putName(b, "Pic", 20), put8(b, 2), put8(b, 0), put16(b, 0);
        put32(b, 32), put32(b, 32 + 8 + 4 + 12 + 24);
        putName(b, "RLPA", 4), put8(b, 1), b.resize(b.size() + 3), put32(b, 12);
        put8(b, 0), put8(b, 5), put8(b, 2), put8(b, 0), put16(b, 2), put16(b, 0), put32(b, 12);
        for (float f : {0.0f, 10.0f, 0.5f, 60.0f, 20.0f, -0.5f}) putf(b, f);
        check(b.size() - pic == 32 + 8 + 4 + 12 + 24, "the fixture's visibility animation sits where its offset says");
        putName(b, "RLVI", 4), put8(b, 1), b.resize(b.size() + 3), put32(b, 12);
        put8(b, 0), put8(b, 0), put8(b, visibilityCurve), put8(b, 0), put16(b, 2), put16(b, 0), put32(b, 12);
        putf(b, 0.0f), put16(b, 1), put16(b, 0), putf(b, 30.0f), put16(b, 0), put16(b, 0);
        set32(b, table + 4, std::uint32_t(b.size() - at));
        putName(b, "M0", 20), put8(b, 1), put8(b, 1), put16(b, 0), put32(b, 28);
        putName(b, "RLTP", 4), put8(b, 1), b.resize(b.size() + 3), put32(b, 12);
        put8(b, 0), put8(b, 0), put8(b, 1), put8(b, 0), put16(b, 1), put16(b, 0), put32(b, 12);
        putf(b, 5.0f), put16(b, 0), put16(b, 0);
    });
    finish(b);
    return b;
}

void animations() {
    using namespace nw4r::lyt;
    Bytes b = animation();
    const Bytes original = b;
    check(!smg::nativeBrlan(b.data(), b.size()), "an animation with every kind on the disc turns native");
    const std::uint8_t* d = b.data();
    const auto* tag = reinterpret_cast<const res::AnimationTagBlock*>(d + 16);
    check(tag->groupNum == 1 && tag->endFrame == 60 && tag->flag == 1 && std::strcmp(reinterpret_cast<const char*>(tag) + tag->nameOffset, "Tag") == 0 &&
          std::strcmp(reinterpret_cast<const char*>(tag) + tag->groupsOffset, "G") == 0, "the tag block reads through res::AnimationTagBlock");
    const auto* info = reinterpret_cast<const res::AnimationBlock*>(d + 16 + tag->blockHeader.size);
    const auto* base = reinterpret_cast<const std::uint8_t*>(info);
    const auto* files = reinterpret_cast<const std::uint32_t*>(info + 1);
    check(info->frameSize == 60 && info->loop == 1 && info->fileNum == 1 && info->animContNum == 2 &&
          std::strcmp(reinterpret_cast<const char*>(files) + files[0], "b.tpl") == 0, "the animation block and its texture file names read natively");
    const auto* contents = reinterpret_cast<const std::uint32_t*>(base + info->animContOffsetsOffset);
    const auto* pic = reinterpret_cast<const res::AnimationContent*>(base + contents[0]);
    const auto* infos = reinterpret_cast<const std::uint32_t*>(pic + 1);
    const auto* srt = reinterpret_cast<const res::AnimationInfo*>(reinterpret_cast<const std::uint8_t*>(pic) + infos[0]);
    const auto* srtTarget = reinterpret_cast<const res::AnimationTarget*>(
        reinterpret_cast<const std::uint8_t*>(srt) + reinterpret_cast<const std::uint32_t*>(srt + 1)[0]);
    const auto* hermite = reinterpret_cast<const res::HermiteKey*>(reinterpret_cast<const std::uint8_t*>(srtTarget) + srtTarget->keysOffset);
    check(std::strcmp(pic->name, "Pic") == 0 && pic->num == 2 && srt->kind == res::ANIMATIONTYPE_RLPA && srtTarget->target == 5 &&
          srtTarget->keyNum == 2 && hermite[1].frame == 60 && hermite[1].value == 20 && hermite[1].slope == -0.5f,
          "a pane's SRT animation reads its Hermite keys as nw4r does");
    const auto* vis = reinterpret_cast<const res::AnimationInfo*>(reinterpret_cast<const std::uint8_t*>(pic) + infos[1]);
    const auto* visTarget = reinterpret_cast<const res::AnimationTarget*>(
        reinterpret_cast<const std::uint8_t*>(vis) + reinterpret_cast<const std::uint32_t*>(vis + 1)[0]);
    const auto* step = reinterpret_cast<const res::StepKey*>(reinterpret_cast<const std::uint8_t*>(visTarget) + visTarget->keysOffset);
    check(vis->kind == res::ANIMATIONTYPE_RLVI && step[0].value == 1 && step[1].frame == 30 && step[1].value == 0, "visibility reads its step keys");
    const auto* m0 = reinterpret_cast<const res::AnimationContent*>(base + contents[1]);
    check(m0->type == 1 && reinterpret_cast<const res::AnimationInfo*>(reinterpret_cast<const std::uint8_t*>(m0) + 28)->kind == res::ANIMATIONTYPE_RLTP,
          "a material's texture pattern reads its kind");
    const Bytes once = b;
    check(!smg::nativeBrlan(b.data(), b.size()) && b == once && b != original, "a native animation is left as it is");

    b = animation(2);
    const Bytes before = b;
    check(smg::nativeBrlan(b.data(), b.size()) && b == before, "a curve its reader would take as another kind is refused");
}

// One I8 texture with a palette header (as C8 would have), its texels and palette as the GPU reads them.
Bytes palette() {
    Bytes b;
    put32(b, 2142000), put32(b, 1), put32(b, 12), put32(b, 20), put32(b, 0x40);
    put16(b, 4), put16(b, 8), put32(b, 9), put32(b, 0x60), put32(b, 0), put32(b, 1), put32(b, 1), put32(b, 1), putf(b, 0.5f);
    put8(b, 0), put8(b, 0), put8(b, 2), put8(b, 0);
    b.resize(0x40);
    put16(b, 2), put8(b, 0), put8(b, 0), put32(b, 2), put32(b, 0x80);
    b.resize(0x60);
    for (int i = 0; i < 32; ++i) put8(b, i);
    put16(b, 0x8000), put16(b, 0x7FFF);
    return b;
}

void textures() {
    Bytes b = palette();
    const Bytes original = b;
    check(!smg::nativeTpl(b.data(), b.size()), "a texture palette turns native");
    const std::uint8_t* d = b.data();
    check(native32(d) == 2142000 && native32(d + 4) == 1 && native32(d + 8) == 12 && native32(d + 12) == 20 && native32(d + 16) == 0x40,
          "the palette and its descriptor read natively, as TPLBind takes them");
    check(native16(d + 20) == 4 && native16(d + 22) == 8 && native32(d + 24) == 9 && native32(d + 28) == 0x60 && native32(d + 36) == 1 &&
          nativef(d + 48) == 0.5f && d[54] == 2 && d[55] == 0, "the texture header reads natively, with the Wii's 4-byte enums");
    check(native16(d + 0x40) == 2 && native32(d + 0x44) == 2 && native32(d + 0x48) == 0x80, "the palette header reads natively");
    check(std::memcmp(d + 0x60, original.data() + 0x60, 36) == 0, "texels and palette entries stay as the GPU reads them");
    const Bytes once = b;
    check(!smg::nativeTpl(b.data(), b.size()) && b == once, "a native texture palette is left as it is");
    b = original;
    b[55] = 1;
    check(smg::nativeTpl(b.data(), b.size()) != nullptr, "a texture already bound is refused");
}

// A font with every block the disc's fonts have, and all three code map methods.
Bytes font() {
    Bytes b = file("RFNT", 6);
    std::size_t finf = 0, tglp = 0, cwdh = 0, direct = 0, table = 0, scan = 0;
    block(b, "FINF", [&](std::size_t at) {
        finf = at;
        put8(b, 1), put8(b, 26), put16(b, 3), put8(b, 0xFF), put8(b, 10), put8(b, 11), put8(b, 1);
        put32(b, 0), put32(b, 0), put32(b, 0), put8(b, 26), put8(b, 24), put8(b, 22), put8(b, 0);
    });
    block(b, "TGLP", [&](std::size_t at) {
        tglp = at;
        put8(b, 24), put8(b, 26), put8(b, 22), put8(b, 24), put32(b, 32), put16(b, 1), put16(b, 0), put16(b, 2), put16(b, 1), put16(b, 64),
            put16(b, 8), put32(b, 0);
        set32(b, at + 28, std::uint32_t(b.size()));
        for (int i = 0; i < 32; ++i) put8(b, 0xA0 + i);
    });
    block(b, "CWDH", [&](std::size_t at) {
        cwdh = at;
        put16(b, 0), put16(b, 2), put32(b, 0);
        for (int i = 0; i < 3; ++i) put8(b, 0xFE), put8(b, 12), put8(b, 13);
    });
    block(b, "CMAP", [&](std::size_t at) { direct = at; put16(b, 0x20), put16(b, 0x22), put16(b, 0), put16(b, 0), put32(b, 0), put16(b, 5); });
    block(b, "CMAP", [&](std::size_t at) { table = at; put16(b, 0x30), put16(b, 0x31), put16(b, 1), put16(b, 0), put32(b, 0), put16(b, 7), put16(b, 0xFFFF); });
    block(b, "CMAP", [&](std::size_t at) { scan = at; put16(b, 0x41), put16(b, 0xFFFF), put16(b, 2), put16(b, 0), put32(b, 0), put16(b, 1), put16(b, 0x3042), put16(b, 2); });
    set32(b, finf + 16, std::uint32_t(tglp + 8)), set32(b, finf + 20, std::uint32_t(cwdh + 8)), set32(b, finf + 24, std::uint32_t(direct + 8));
    set32(b, direct + 16, std::uint32_t(table + 8)), set32(b, table + 16, std::uint32_t(scan + 8));
    finish(b);
    return b;
}

void fonts() {
    Bytes b = font();
    const Bytes original = b;
    check(!smg::nativeBrfnt(b.data(), b.size()), "a font with all three code map methods turns native");
    const std::uint8_t* d = b.data();
    check(native32(d) == 0x52464E54 && native16(d + 4) == 0xFEFF && native16(d + 6) == 0x104 && native32(d + 8) == b.size() &&
          native16(d + 14) == 6, "the header reads as IsValidBinaryFile takes it");
    const std::size_t finf = 16;
    check(native32(d + finf) == 0x46494E46 && d[finf + 9] == 26 && native16(d + finf + 10) == 3 && d[finf + 12] == 0xFF && d[finf + 15] == 1 &&
          d[finf + 30] == 22, "font information reads natively, its widths and sizes as bytes");
    const std::size_t tglp = native32(d + finf + 16) - 8, cwdh = native32(d + finf + 20) - 8, direct = native32(d + finf + 24) - 8;
    check(native32(d + tglp) == 0x54474C50 && d[tglp + 9] == 26 && native32(d + tglp + 12) == 32 && native16(d + tglp + 20) == 2 &&
          native16(d + tglp + 26) == 8 && std::memcmp(d + native32(d + tglp + 28), original.data() + native32(d + tglp + 28), 32) == 0,
          "the glyph sheet header reads natively and its sheet stays as the GPU reads it");
    check(native16(d + cwdh + 10) == 2 && d[cwdh + 16] == 0xFE && d[cwdh + 24] == 13, "a width table reads natively, its widths as bytes");
    const std::size_t table = native32(d + direct + 16) - 8, scan = native32(d + table + 16) - 8;
    check(native16(d + direct + 20) == 5 && native16(d + table + 12) == 1 && native16(d + table + 22) == 0xFFFF && native16(d + scan + 12) == 2 &&
          native16(d + scan + 22) == 0x3042 && native16(d + scan + 24) == 2, "direct, table and scan code maps read natively");
    const Bytes once = b;
    check(!smg::nativeBrfnt(b.data(), b.size()) && b == once, "a native font is left as it is");
    std::memcpy(b.data(), "RFNU", 4);
    std::reverse(b.begin(), b.begin() + 4);
    check(!smg::nativeBrfnt(b.data(), b.size()), "a font nw4r has already rebuilt is left as it is");
    b = original;
    b[direct + 13] = 3;
    check(smg::nativeBrfnt(b.data(), b.size()) != nullptr, "a code map method the port has no layout for is refused");
}
} // namespace

int runLayoutTests() {
    layouts();
    refusesLayouts();
    animations();
    textures();
    fonts();
    std::printf("%u layout, animation, texture and font checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runLayoutTests(); }
#endif
