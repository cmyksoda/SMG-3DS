// resource_audit FILES --layouts: every BRLYT, BRLAN, TPL and BRFNT on the disc (and in the game's embedded error
// archive) turned native, then read back through nw4r's own resource structs and getters and compared with a
// big-endian read of the original.
#include <Game/System/ErrorArchive.hpp>
#include <nw4r/lyt/common.h>
#include <nw4r/lyt/resources.h>
#include <nw4r/ut/Color.h>
#include <smg/Archive.hpp>
#include <smg/Layout.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace nw4r::lyt;

namespace {
using Bytes = std::vector<std::uint8_t>;
std::uint32_t be16(const std::uint8_t* at) { return std::uint32_t(at[0]) << 8 | at[1]; }
std::uint32_t be32(const std::uint8_t* at) { return be16(at) << 16 | be16(at + 2); }
std::uint32_t n16(const std::uint8_t* at) { std::uint16_t v; std::memcpy(&v, at, 2); return v; }
std::uint32_t n32(const std::uint8_t* at) { std::uint32_t v; std::memcpy(&v, at, 4); return v; }
std::uint32_t bits(float value) { std::uint32_t v; std::memcpy(&v, &value, 4); return v; }
// lyt_common.cpp's TestFileHeader, whose file the host tools don't build.
bool testFileHeader(const res::BinaryFileHeader& header, u32 signature) {
    return u32(detail::GetSignatureInt(header.signature)) == signature && header.byteOrder == 0xFEFF;
}
std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

struct Audit {
    std::map<std::string, std::size_t> counts;
    std::size_t files = 0, fields = 0, wrong = 0, refused = 0;
    const std::uint8_t* o = nullptr;  // the original, big-endian
    const std::uint8_t* n = nullptr;  // the same file turned native
    std::string where;

    void same(bool agree, const char* what, std::size_t at) {
        ++fields;
        if (!agree && wrong++ < 30) std::printf("%s: %s differs at 0x%zx\n", where.c_str(), what, at);
    }
    void word(std::uint32_t native, std::size_t at, const char* what) { same(native == be32(o + at), what, at); }
    void half(std::uint32_t native, std::size_t at, const char* what) { same(native == be16(o + at), what, at); }
    void f32(float native, std::size_t at, const char* what) { same(bits(native) == be32(o + at), what, at); }
    void bytes(const void* native, std::size_t at, std::size_t count, const char* what) {
        same(std::memcmp(native, o + at, count) == 0, what, at);
    }
    std::size_t at(const void* p) const { return static_cast<const std::uint8_t*>(p) - n; }
    // A u32 color, taken as nw4r takes it: into a ut::Color by value, whose bytes the Wii's memory held.
    void color(u32 native, std::size_t offset, const char* what) {
        const nw4r::ut::Color c(native);
        const std::uint8_t rgba[4] = {c.r, c.g, c.b, c.a};
        bytes(rgba, offset, 4, what);
    }
    void pane(const res::Pane* p) {
        const std::size_t a = at(p);
        bytes(&p->flag, a + 8, 28, "a pane's flags, name and user data");
        const float* f = &p->translate.x;
        for (int i = 0; i < 10; ++i) f32(f[i], a + 36 + 4 * i, "a pane's transform or size");
        ++counts["panes"];
    }
    void coords(const std::uint8_t* start, unsigned count) {
        for (unsigned i = 0; i < 8 * count; ++i) f32(reinterpret_cast<const float*>(start)[i], at(start) + 4 * i, "a texture coordinate");
    }
};

void brlyt(Audit& a) {
    const auto* file = reinterpret_cast<const res::BinaryFileHeader*>(a.n);
    a.word(n32(a.n), 0, "the signature");
    a.half(file->byteOrder, 4, "the byte order mark"), a.half(file->version, 6, "the version"), a.word(file->fileSize, 8, "the file size");
    a.same(testFileHeader(*file, res::FILESIGNATURE_RLYT) && detail::TestFileVersion(*file), "nw4r's own header test", 0);
    const res::MaterialList* materials = nullptr;
    const std::uint8_t* block = a.n + file->headerSize;
    for (int i = 0; i < file->dataBlocks; ++i, block += reinterpret_cast<const res::DataBlockHeader*>(block)->size) {
        const auto* head = reinterpret_cast<const res::DataBlockHeader*>(block);
        const u32 kind = detail::GetSignatureInt(head->kind);
        a.word(kind, a.at(block), "a block kind"), a.word(head->size, a.at(block) + 4, "a block size");
        char name[5] = {};
        std::memcpy(name, a.o + a.at(block), 4);
        ++a.counts[std::string("BRLYT ") + name];
        if (kind == res::DATABLOCKKIND_LAYOUT) {
            const auto* l = reinterpret_cast<const res::Layout*>(block);
            a.same(l->originType == a.o[a.at(block) + 8], "the origin type", a.at(block) + 8);
            a.f32(l->layoutSize.width, a.at(&l->layoutSize.width), "the layout size"), a.f32(l->layoutSize.height, a.at(&l->layoutSize.height), "the layout size");
        } else if (kind == res::DATABLOCKKIND_TEXTURELIST || kind == res::DATABLOCKKIND_FONTLIST) {
            const auto* list = reinterpret_cast<const res::TextureList*>(block);
            a.half(list->texNum, a.at(&list->texNum), "a name list's count");
            const auto* entries = reinterpret_cast<const res::Texture*>(list + 1);
            for (int e = 0; e < list->texNum; ++e) {
                a.word(entries[e].nameStrOffset, a.at(&entries[e]), "a name offset");
                const char* entryName = detail::ConvertOffsToPtr<char>(entries, entries[e].nameStrOffset);
                a.bytes(entryName, a.at(entryName), std::strlen(entryName) + 1, "a texture or font name");
            }
        } else if (kind == res::DATABLOCKKIND_MATERIALLIST) {
            materials = reinterpret_cast<const res::MaterialList*>(block);
            a.half(materials->materialNum, a.at(&materials->materialNum), "the material count");
            const u32* offsets = detail::ConvertOffsToPtr<u32>(materials, sizeof(res::MaterialList));
            for (int m = 0; m < materials->materialNum; ++m) {
                a.word(offsets[m], a.at(&offsets[m]), "a material offset");
                const auto* mat = detail::ConvertOffsToPtr<res::Material>(materials, offsets[m]);
                const std::size_t base = a.at(mat);
                a.bytes(mat->name, base, 20, "a material name");
                for (int c = 0; c < 3; ++c) {
                    const s16* v = &mat->tevCols[c].r;
                    for (int k = 0; k < 4; ++k) a.half(u16(v[k]), base + 20 + 8 * c + 2 * k, "a TEV color");
                }
                a.bytes(mat->tevKCols, base + 44, 16, "the TEV constant colors");
                a.word(mat->resNum.bits, base + 60, "the resource bits");
                ++a.counts["materials"];
                // In lyt_material.cpp's order, by nw4r's own getters.
                std::size_t r = base + sizeof(res::Material);
                const auto run = [&](unsigned count, std::size_t size, auto each) {
                    for (unsigned k = 0; k < count; ++k, r += size) each(a.n + r);
                };
                const res::MaterialResourceNum& num = mat->resNum;
                run(num.GetTexMapNum(), 4, [&](const std::uint8_t* p) {
                    const auto* t = reinterpret_cast<const res::TexMap*>(p);
                    a.half(t->texIdx, a.at(p), "a texture map index"), a.bytes(&t->wrapSflt, a.at(p) + 2, 2, "a texture map's wrap and filter");
                });
                const auto srt = [&](const std::uint8_t* p) {
                    const auto* t = reinterpret_cast<const TexSRT*>(p);
                    const float* f = &t->translate.x;
                    for (int k = 0; k < 5; ++k) a.f32(f[k], a.at(p) + 4 * k, "a texture SRT");
                };
                const auto four = [&](const std::uint8_t* p) { a.bytes(p, a.at(p), 4, "a byte-wise material resource"); };
                run(num.GetTexSRTNum(), 20, srt);
                run(num.GetTexCoordGenNum(), 4, four);
                run(num.GetChanCtrlNum(), 4, four);
                run(num.GetMatColNum(), 4, four);
                run(num.HasTevSwapTable(), 4, four);
                run(num.GetIndTexSRTNum(), 20, srt);
                run(num.GetIndTexStageNum(), 4, four);
                run(num.GetTevStageNum(), 16, [&](const std::uint8_t* p) { a.bytes(p, a.at(p), 16, "a TEV stage"); });
                run(num.HasAlphaCompare(), 4, four);
                run(num.HasBlendMode(), 4, four);
                a.counts["material resources"] += num.GetTexMapNum() + num.GetTexSRTNum() + num.GetTexCoordGenNum() + num.GetIndTexSRTNum() +
                                                  num.GetIndTexStageNum() + num.GetTevStageNum();
            }
        } else if (kind == res::DATABLOCKKIND_PANE || kind == res::DATABLOCKKIND_BOUNDING) {
            a.pane(reinterpret_cast<const res::Pane*>(block));
        } else if (kind == res::DATABLOCKKIND_PICTURE) {
            const auto* p = reinterpret_cast<const res::Picture*>(block);
            a.pane(p);
            for (int c = 0; c < 4; ++c) a.color(p->vtxCols[c], a.at(&p->vtxCols[c]), "a picture's vertex color");
            a.half(p->materialIdx, a.at(&p->materialIdx), "a picture's material");
            a.same(p->texCoordNum == a.o[a.at(&p->texCoordNum)] && materials && p->materialIdx < materials->materialNum, "a picture's coordinate count",
                   a.at(&p->texCoordNum));
            a.coords(block + sizeof(res::Picture), p->texCoordNum);
        } else if (kind == res::DATABLOCKKIND_TEXTBOX) {
            const auto* t = reinterpret_cast<const res::TextBox*>(block);
            a.pane(t);
            a.half(t->textBufBytes, a.at(&t->textBufBytes), "a text buffer size"), a.half(t->textStrBytes, a.at(&t->textStrBytes), "a text size");
            a.half(t->materialIdx, a.at(&t->materialIdx), "a text box's material"), a.half(t->fontIdx, a.at(&t->fontIdx), "a text box's font");
            a.bytes(&t->textPosition, a.at(&t->textPosition), 2, "a text position and alignment");
            a.word(t->textStrOffset, a.at(&t->textStrOffset), "a text offset");
            for (int c = 0; c < 2; ++c) a.color(t->textCols[c], a.at(&t->textCols[c]), "a text color");
            a.f32(t->fontSize.width, a.at(&t->fontSize.width), "a font size"), a.f32(t->fontSize.height, a.at(&t->fontSize.height), "a font size");
            a.f32(t->charSpace, a.at(&t->charSpace), "character spacing"), a.f32(t->lineSpace, a.at(&t->lineSpace), "line spacing");
            const auto* text = detail::ConvertOffsToPtr<u16>(t, t->textStrOffset);
            for (int u = 0; u < t->textStrBytes / 2; ++u) a.half(text[u], a.at(&text[u]), "a text unit");
            a.counts["text units"] += t->textStrBytes / 2;
        } else if (kind == res::DATABLOCKKIND_WINDOW) {
            const auto* w = reinterpret_cast<const res::Window*>(block);
            a.pane(w);
            const float* inflation = &w->inflation.l;
            for (int k = 0; k < 4; ++k) a.f32(inflation[k], a.at(&inflation[k]), "a window's inflation");
            a.same(w->frameNum == a.o[a.at(&w->frameNum)], "a window's frame count", a.at(&w->frameNum));
            a.word(w->contentOffset, a.at(&w->contentOffset), "a window's content offset");
            a.word(w->frameOffsetTableOffset, a.at(&w->frameOffsetTableOffset), "a window's frame table offset");
            const auto* content = detail::ConvertOffsToPtr<res::WindowContent>(w, w->contentOffset);
            for (int c = 0; c < 4; ++c) a.color(content->vtxCols[c], a.at(&content->vtxCols[c]), "a window's vertex color");
            a.half(content->materialIdx, a.at(&content->materialIdx), "a window's material");
            a.coords(reinterpret_cast<const std::uint8_t*>(content + 1), content->texCoordNum);
            const u32* frames = detail::ConvertOffsToPtr<u32>(w, w->frameOffsetTableOffset);
            for (int f = 0; f < w->frameNum; ++f) {
                a.word(frames[f], a.at(&frames[f]), "a window frame offset");
                const auto* frame = detail::ConvertOffsToPtr<res::WindowFrame>(w, frames[f]);
                a.half(frame->materialIdx, a.at(frame), "a window frame's material"), a.bytes(&frame->textureFlip, a.at(frame) + 2, 1, "a frame's flip");
            }
        } else if (kind == res::DATABLOCKKIND_GROUP) {
            const auto* g = reinterpret_cast<const res::Group*>(block);
            a.bytes(g->name, a.at(g->name), 16, "a group name"), a.half(g->paneNum, a.at(&g->paneNum), "a group's pane count");
            a.bytes(g + 1, a.at(g + 1), 16 * g->paneNum, "a group's pane names");
        }
    }
}

void brlan(Audit& a) {
    const auto* file = reinterpret_cast<const res::BinaryFileHeader*>(a.n);
    a.word(n32(a.n), 0, "the signature"), a.word(file->fileSize, 8, "the file size");
    a.same(testFileHeader(*file, res::FILESIGNATURE_RLAN) && detail::TestFileVersion(*file), "nw4r's own header test", 0);
    const std::uint8_t* block = a.n + file->headerSize;
    for (int i = 0; i < file->dataBlocks; ++i, block += reinterpret_cast<const res::DataBlockHeader*>(block)->size) {
        const u32 kind = detail::GetSignatureInt(reinterpret_cast<const res::DataBlockHeader*>(block)->kind);
        a.word(kind, a.at(block), "a block kind");
        if (kind == res::DATABLOCKKIND_PANEANIMTAG) {
            const auto* t = reinterpret_cast<const res::AnimationTagBlock*>(block);
            a.half(t->groupNum, a.at(&t->groupNum), "a tag's group count"), a.word(t->nameOffset, a.at(&t->nameOffset), "a tag's name offset");
            a.half(u16(t->startFrame), a.at(&t->startFrame), "a tag's start"), a.half(u16(t->endFrame), a.at(&t->endFrame), "a tag's end");
            const char* name = detail::ConvertOffsToPtr<char>(t, t->nameOffset);
            a.bytes(name, a.at(name), std::strlen(name) + 1, "a tag's name");
            ++a.counts["BRLAN tags"];
        } else if (kind == res::DATABLOCKKIND_PANEANIMINFO) {
            const auto* b = reinterpret_cast<const res::AnimationBlock*>(block);
            a.half(b->frameSize, a.at(&b->frameSize), "an animation's frame count"), a.same(b->loop == a.o[a.at(&b->loop)], "loop", a.at(&b->loop));
            a.half(b->fileNum, a.at(&b->fileNum), "a file count"), a.half(b->animContNum, a.at(&b->animContNum), "a content count");
            const u32* names = detail::ConvertOffsToPtr<u32>(b, sizeof(*b));
            for (int f = 0; f < b->fileNum; ++f) {
                const char* name = detail::GetStrTableStr(names, f);
                a.bytes(name, a.at(name), std::strlen(name) + 1, "a texture file name");
            }
            const u32* contents = detail::ConvertOffsToPtr<u32>(b, b->animContOffsetsOffset);
            for (int c = 0; c < b->animContNum; ++c) {
                const auto* content = detail::ConvertOffsToPtr<res::AnimationContent>(b, contents[c]);
                a.bytes(content, a.at(content), sizeof(*content), "an animation content");
                const u32* infos = detail::ConvertOffsToPtr<u32>(content, sizeof(*content));
                for (int k = 0; k < content->num; ++k) {
                    const auto* info = detail::ConvertOffsToPtr<res::AnimationInfo>(content, infos[k]);
                    a.word(info->kind, a.at(info), "an animation kind");
                    char kindName[5] = {};
                    std::memcpy(kindName, a.o + a.at(info), 4);
                    const bool step = info->kind == res::ANIMATIONTYPE_RLVI || info->kind == res::ANIMATIONTYPE_RLTP;
                    const u32* targets = detail::ConvertOffsToPtr<u32>(info, sizeof(*info));
                    for (int t = 0; t < info->num; ++t) {
                        const auto* target = detail::ConvertOffsToPtr<res::AnimationTarget>(info, targets[t]);
                        a.bytes(target, a.at(target), 4, "a target's id, target and curve");
                        a.half(target->keyNum, a.at(&target->keyNum), "a key count");
                        ++a.counts[std::string("BRLAN ") + kindName + " targets"];
                        a.counts["BRLAN keys"] += target->keyNum;
                        for (int key = 0; key < target->keyNum; ++key) {
                            if (step) {
                                const auto& s = detail::ConvertOffsToPtr<res::StepKey>(target, target->keysOffset)[key];
                                a.f32(s.frame, a.at(&s.frame), "a step key's frame"), a.half(s.value, a.at(&s.value), "a step key's value");
                            } else {
                                const auto& h = detail::ConvertOffsToPtr<res::HermiteKey>(target, target->keysOffset)[key];
                                a.f32(h.frame, a.at(&h.frame), "a key's frame"), a.f32(h.value, a.at(&h.value), "a key's value");
                                a.f32(h.slope, a.at(&h.slope), "a key's slope");
                            }
                        }
                    }
                }
            }
        }
    }
}

// TPLPalette, TPLDescriptor and TPLHeader by offset: their pointers are 8 bytes on a 64-bit host.
void tpl(Audit& a, std::size_t size) {
    a.word(n32(a.n), 0, "the version"), a.word(n32(a.n + 4), 4, "the texture count"), a.word(n32(a.n + 8), 8, "the descriptor offset");
    for (std::uint32_t i = 0; i < n32(a.n + 4); ++i) {
        const std::size_t d = n32(a.n + 8) + 8 * i, h = n32(a.n + d), c = n32(a.n + d + 4);
        a.word(h, d, "a texture header offset"), a.word(c, d + 4, "a palette header offset");
        if (h) {
            a.half(n16(a.n + h), h, "a height"), a.half(n16(a.n + h + 2), h + 2, "a width");
            for (int k = 1; k < 8; ++k) a.word(n32(a.n + h + 4 * k), h + 4 * k, "a texture header word");
            a.bytes(a.n + h + 32, h + 32, 4, "the LOD bytes");
            ++a.counts["TPL format " + std::to_string(n32(a.n + h + 4))];
        }
        if (c) a.half(n16(a.n + c), c, "a palette size"), a.word(n32(a.n + c + 4), c + 4, "a palette format");
    }
    const std::size_t texels = n32(a.n + n32(a.n + n32(a.n + 8)) + 8);
    a.bytes(a.n + texels, texels, size - texels, "the texels");
}

// ut::FontInformation and friends by offset, for the same reason.
void brfnt(Audit& a) {
    a.word(n32(a.n), 0, "the signature"), a.half(n16(a.n + 6), 6, "the version"), a.word(n32(a.n + 8), 8, "the file size");
    std::size_t at = n16(a.n + 12);
    for (unsigned i = 0; i < n16(a.n + 14); ++i, at += n32(a.n + at + 4)) {
        const std::uint32_t kind = n32(a.n + at);
        a.word(kind, at, "a block kind");
        char name[5] = {};
        std::memcpy(name, a.o + at, 4);
        ++a.counts[std::string("BRFNT ") + name];
        if (kind == 0x46494E46) {  // FINF
            a.half(n16(a.n + at + 10), at + 10, "the alternate glyph");
            for (int k = 0; k < 3; ++k) a.word(n32(a.n + at + 16 + 4 * k), at + 16 + 4 * k, "a font pointer");
        } else if (kind == 0x54474C50) {  // TGLP
            a.word(n32(a.n + at + 12), at + 12, "a sheet size");
            for (int k = 0; k < 6; ++k) a.half(n16(a.n + at + 16 + 2 * k), at + 16 + 2 * k, "a sheet field");
            const std::size_t image = n32(a.n + at + 28);
            a.bytes(a.n + image, image, std::size_t(n32(a.n + at + 12)) * n16(a.n + at + 16), "the glyph sheets");
        } else if (kind == 0x43574448) {  // CWDH
            a.half(n16(a.n + at + 8), at + 8, "a width range"), a.half(n16(a.n + at + 10), at + 10, "a width range");
        } else if (kind == 0x434D4150) {  // CMAP
            const unsigned method = n16(a.n + at + 12);
            a.half(method, at + 12, "a code map method");
            ++a.counts["BRFNT code maps, method " + std::to_string(method)];
            const std::size_t units = method == 0 ? 1 : method == 1 ? n16(a.n + at + 10) - n16(a.n + at + 8) + 1 : 1 + 2 * n16(a.n + at + 20);
            for (std::size_t u = 0; u < units; ++u) a.half(n16(a.n + at + 20 + 2 * u), at + 20 + 2 * u, "a code map entry");
        }
    }
}

void auditFile(Audit& a, const std::string& where, const std::string& name, const std::uint8_t* data, std::size_t size) {
    const std::string ext = fs::path(lower(name)).extension().string();
    const char* (*turn)(std::uint8_t*, std::size_t) = ext == ".brlyt" ? smg::nativeBrlyt : ext == ".brlan" ? smg::nativeBrlan
                                                     : ext == ".tpl"   ? smg::nativeTpl   : ext == ".brfnt" ? smg::nativeBrfnt : nullptr;
    if (!turn) return;
    ++a.files, ++a.counts[ext + " files"];
    a.where = where + " " + name;
    Bytes copy(data, data + size);
    if (const char* why = turn(copy.data(), copy.size())) {
        std::printf("%s: %s\n", a.where.c_str(), why);
        ++a.refused;
        return;
    }
    Bytes again = copy;
    a.same(turn(again.data(), again.size()) == nullptr && again == copy, "a second turn", 0);
    a.o = data, a.n = copy.data();
    if (ext == ".brlyt") brlyt(a);
    else if (ext == ".brlan") brlan(a);
    else if (ext == ".tpl") tpl(a, size);
    else brfnt(a);
}

// Archives inside an archive (the error archive's layouts and fonts) are audited as well.
void auditArchive(Audit& a, const std::string& where, smg::Bytes source, int depth) {
    std::vector<std::uint8_t> storage;
    std::vector<smg::ArchiveEntry> entries;
    std::string error;
    if (source.size >= 4 && std::memcmp(source.data, "Yaz0", 4) == 0) {
        if (!smg::decodeYaz0(source, storage, 64u << 20, error)) return;
        source = {storage.data(), storage.size()};
    }
    if (!smg::indexRarc(source, entries, error)) return;
    for (const auto& entry : entries) {
        const std::uint8_t* data = source.data + entry.offset;
        if (depth == 0 && lower(entry.path).size() > 4 && lower(entry.path).substr(lower(entry.path).size() - 4) == ".arc")
            auditArchive(a, where + " " + entry.path, {data, entry.size}, depth + 1);
        else auditFile(a, where, entry.path, data, entry.size);
    }
}
} // namespace

int auditLayouts(const fs::path& root) {
    Audit a;
    std::vector<fs::path> archives;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && lower(entry.path().extension().string()) == ".arc") archives.push_back(entry.path());
    std::sort(archives.begin(), archives.end());
    for (const auto& path : archives) {
        std::vector<std::uint8_t> raw;
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            raw.resize(fs::file_size(path));
            raw.resize(std::fread(raw.data(), 1, raw.size(), f));
            std::fclose(f);
        }
        auditArchive(a, fs::relative(path, root).string(), {raw.data(), raw.size()}, 0);
    }
    auditArchive(a, "(the game's embedded error archive)", {cErrorArchive, sizeof cErrorArchive}, 0);
    for (const auto& [what, count] : a.counts) std::printf("  %s: %zu\n", what.c_str(), count);
    std::printf("%zu files, %zu refused, %zu fields compared, %zu differ\n", a.files, a.refused, a.fields, a.wrong);
    return a.refused || a.wrong ? 1 : 0;
}
