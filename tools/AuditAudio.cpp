// resource_audit FILES --audio: the disc's audio data turned native (SMR.szs's archive, the chord, music-event, remix
// and speaker files), then read back through JAudio2's own structs, as its parsers walk them, and compared with a
// big-endian read of the original.
#include <JSystem/JAudio2/JASBNKParser.hpp>
#include <JSystem/JAudio2/JASWSParser.hpp>
#include <JSystem/JAudio2/JAUSeqCollection.hpp>
#include <JSystem/JAudio2/JAUSoundTable.hpp>
#include <smg/Archive.hpp>
#include <smg/AudioData.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
using Bytes = std::vector<std::uint8_t>;
std::uint32_t be16(const std::uint8_t* at) { return std::uint32_t(at[0]) << 8 | at[1]; }
std::uint32_t be32(const std::uint8_t* at) { return be16(at) << 16 | be16(at + 2); }
constexpr std::uint32_t tag(const char (&text)[5]) {
    return std::uint32_t(std::uint8_t(text[0])) << 24 | std::uint32_t(std::uint8_t(text[1])) << 16 |
           std::uint32_t(std::uint8_t(text[2])) << 8 | std::uint8_t(text[3]);
}

struct Audit {
    std::size_t files = 0, refused = 0, fields = 0, wrong = 0, implausible = 0;
    std::map<std::string, std::size_t> counts;
    const std::uint8_t* before = nullptr;  // the original, big-endian
    const std::uint8_t* after = nullptr;   // the converted copy
    std::string where;

    std::size_t at(const void* field) const { return static_cast<const std::uint8_t*>(field) - after; }
    void same(std::uint32_t native, std::uint32_t expected, const char* what) {
        ++fields;
        if (native != expected && wrong++ < 20) std::printf("%s: %s reads 0x%x, big-endian 0x%x\n", where.c_str(), what, native, expected);
    }
    // A field read natively from the copy, against the original's bytes where it lies.
    template <typename T> void word(const T& field, const char* what) {
        static_assert(sizeof(T) == 4);
        std::uint32_t native;
        std::memcpy(&native, &field, 4);
        same(native, be32(before + at(&field)), what);
    }
    template <typename T> void half(const T& field, const char* what) {
        static_assert(sizeof(T) == 2);
        same(static_cast<std::uint16_t>(field), be16(before + at(&field)), what);
    }
    void byte(const std::uint8_t& field, const char* what) { same(field, before[at(&field)], what); }
    void plausible(bool ok, const char* what) {
        if (!ok && implausible++ < 20) std::printf("%s: %s is implausible\n", where.c_str(), what);
    }
};

template <typename T> const T* in(const std::uint8_t* base, std::uint32_t offset) { return reinterpret_cast<const T*>(base + offset); }

// JAUSoundTable_ as JAUSoundTable and JAUSoundNameTable walk it.
void soundTable(Audit& a, const std::uint8_t* bst) {
    JAUSoundTable_< JAUSoundTableRoot, JAUSoundTableSection, JAUSoundTableGroup, void > t;
    t.init(bst);
    a.plausible(t.mData != nullptr, "the sound table's signature");
    if (!t.mData) return;
    a.word(t.mRoot->mSectionNumber, "BST sections");
    for (int s = 0; s < int(t.mRoot->mSectionNumber); ++s) {
        a.word(t.mRoot->mSectionOffsets[s], "BST section offset");
        const JAUSoundTableSection* section = t.getSection(s);
        if (!section) continue;
        a.word(section->mNumGroups, "BST groups");
        for (int g = 0; g < int(section->mNumGroups); ++g) {
            a.word(section->mGroupOffsets[g], "BST group offset");
            const JAUSoundTableGroup* group = t.getGroup(section, g);
            if (!group) continue;
            a.word(group->mNumItems, "BST items");
            a.word(group->mGlobalOffset, "BST group global offset");
            for (int i = 0; i < int(group->mNumItems); ++i) {
                a.word(group->mItemOffsets[i], "BST item word");
                a.same(group->getTypeID(i), a.before[a.at(&group->mItemOffsets[i])], "BST item type");
                const void* item = t.getItem(group, i);
                if (!item) continue;
                const auto* common = static_cast<const JAUSoundTableItem*>(item);
                a.byte(common->mPriority, "priority"), a.byte(common->mVolume, "volume");
                switch (group->getTypeID(i) & 0xF0) {
                case 0x50: {
                    const auto* se = static_cast<const JAUSoundTableSe*>(item);
                    a.half(se->mAudibleSw, "SE audible switch"), a.half(se->mSoundSw, "SE sound switch");
                    ++a.counts["sound effects"];
                    break;
                }
                case 0x60: {
                    const auto* bgm = static_cast<const JAUSoundTableBgm*>(item);
                    a.half(bgm->mResourceId, "BGM resource"), a.half(bgm->mChordResId, "BGM chord"), a.half(bgm->mSoundSw, "BGM switch");
                    ++a.counts["music"];
                    break;
                }
                case 0x70: {
                    const auto* stream = static_cast<const JAUSoundTableStream*>(item);
                    a.half(stream->mChannelCtrl, "stream channels"), a.word(stream->mStreamFileNameOffset, "stream name offset");
                    const std::string name(reinterpret_cast<const char*>(bst) + stream->mStreamFileNameOffset);
                    a.plausible(name.size() > 4 && name.substr(name.size() - 4) == ".ast", "a stream's file name");
                    ++a.counts["streams"];
                    break;
                }
                default: a.plausible(false, "a sound's type");
                }
            }
        }
    }
}

void soundNames(Audit& a, const std::uint8_t* bstn) {
    JAUSoundTable_< JAUSoundNameTableRoot, JAUSoundNameTableSection, JAUSoundNameTableGroup, char > t;
    t.init(bstn);
    a.plausible(t.mData != nullptr, "the sound name table's signature");
    if (!t.mData) return;
    a.word(t.mRoot->mSectionNumber, "BSTN sections");
    for (int s = 0; s < int(t.mRoot->mSectionNumber); ++s) {
        const JAUSoundNameTableSection* section = t.getSection(s);
        if (!section) continue;
        a.word(section->mNumGroups, "BSTN groups"), a.word(section->mGlobalOffset, "BSTN section global offset");
        for (int g = 0; g < int(section->mNumGroups); ++g) {
            a.word(section->mGroupOffsets[g], "BSTN group offset");
            const JAUSoundNameTableGroup* group = t.getGroup(section, g);
            if (!group) continue;
            a.word(group->mNumItems, "BSTN items"), a.word(group->mCategoryNameOffset, "BSTN category name");
            for (int i = 0; i < int(group->mNumItems); ++i) {
                a.word(group->mItemOffsets[i], "BSTN item offset");
                if (const char* name = t.getItem(group, i)) {
                    bool printable = *name != 0;
                    for (const char* c = name; *c; ++c) printable &= *c >= 0x20 && *c < 0x7F;
                    a.plausible(printable, "a sound's name");
                    ++a.counts["sound names"];
                }
            }
        }
    }
}

void sequenceCollection(Audit& a, const std::uint8_t* bsc) {
    // As JAUSeqCollection::init and getSeqData read it.
    const auto* data = reinterpret_cast<const JAUSeqCollectionData*>(bsc);
    a.plausible(data->magic[0] == 'S' && data->magic[1] == 'C', "the sequence collection's signature");
    a.half(data->mGroupNum, "BSC groups");
    a.word(data->mFileSize, "BSC size");
    const std::uint32_t* offsets = &data->mSoundTableOffsets;
    for (int g = 0; g < data->mGroupNum; ++g) {
        a.word(offsets[g], "BSC group offset");
        // The port's getSeqData reads these by bytes, big-endian, where they sit.
        const std::uint8_t* table = bsc + offsets[g];
        a.plausible(std::memcmp(table, a.before + a.at(table), 4 + 4 * be32(table)) == 0, "a sequence group (it stays big-endian)");
        for (std::uint32_t w = 0; w < be32(table); ++w) {
            a.plausible(be32(table + 4 + 4 * w) < data->mFileSize, "a sequence's offset");
            ++a.counts["sequences"];
        }
    }
}

// As JASWSParser::createBasicWaveBank walks a wave system.
void waveSystem(Audit& a, const std::uint8_t* wsys) {
    using P = JASWSParser;
    const auto* header = reinterpret_cast<const P::THeader*>(wsys);
    a.word(header->mSize, "WSYS size"), a.word(header->mId, "WSYS id"), a.word(header->mWaveTableSize, "WSYS wave table");
    const P::TCtrlGroup* group = in<P::TCtrlGroup>(wsys, be32(a.before + a.at(wsys) + 0x14));
    const P::TWaveArchiveBank* bank = in<P::TWaveArchiveBank>(wsys, be32(a.before + a.at(wsys) + 0x10));
    a.plausible(header->mCtrlGroupOffset.ptr(header) == group && header->mArchiveBankOffset.ptr(header) == bank, "the header's offsets");
    a.word(group->mGroupCount, "WSYS groups");
    a.word(bank->mWaveGroupCount, "WSYS archive count");
    for (std::uint32_t g = 0; g < group->mGroupCount; ++g) {
        const std::uint8_t* sceneBefore = a.before + a.at(&group->mCtrlSceneOffsets[g]);
        const P::TCtrlScene* scene = group->mCtrlSceneOffsets[g].ptr(wsys);
        a.plausible(a.at(scene) == a.at(wsys) + be32(sceneBefore), "a control scene's offset");
        const P::TCtrl* ctrl = scene->mCtrlOffset.ptr(wsys);
        a.plausible(a.at(ctrl) == a.at(wsys) + be32(a.before + a.at(&scene->mCtrlOffset)), "a control's offset");
        const P::TWaveArchive* archive = bank->mArchiveOffsets[g].ptr(wsys);
        a.plausible(a.at(archive) == a.at(wsys) + be32(a.before + a.at(&bank->mArchiveOffsets[g])), "an archive's offset");
        a.word(ctrl->mWaveCount, "WSYS control waves");
        for (std::uint32_t w = 0; w < ctrl->mWaveCount; ++w) {
            const P::TCtrlWave* id = ctrl->mCtrlWaveOffsets[w].ptr(wsys);
            a.word(id->mData, "wave id word");
            const P::TWave* wave = archive->mWaveOffsets[w].ptr(wsys);
            a.plausible(a.at(wave) == a.at(wsys) + be32(a.before + a.at(&archive->mWaveOffsets[w])), "a wave's offset");
            a.byte(wave->mFormat, "wave format"), a.byte(wave->mBaseKey, "wave base key");
            a.word(wave->mSampleRate, "wave sample rate"), a.word(wave->mAWStartOffs, "wave start"), a.word(wave->mAWLength, "wave length");
            a.word(wave->mLoopFlags, "wave loop flag"), a.word(wave->mSampleLoopStart, "loop start"), a.word(wave->mSampleLoopEnd, "loop end");
            a.word(wave->mSampleCount, "sample count"), a.half(wave->mpLastSample, "last sample"), a.half(wave->mpPenultSample, "penultimate sample");
            a.plausible(wave->mSampleRate >= 1000.0f && wave->mSampleRate <= 48000.0f, "a wave's sample rate");
            a.plausible(!wave->mLoopFlags || (wave->mSampleLoopStart <= wave->mSampleLoopEnd && wave->mSampleLoopEnd <= wave->mSampleCount),
                        "a wave's loop");
            ++a.counts["waves"];
        }
    }
}

// As JASBNKParser::Ver1::createBasicBank walks a bank.
void bankVelocities(Audit& a, const std::uint32_t*& data, std::uint32_t count) {
    for (std::uint32_t v = 0; v < count; ++v, data += 4) {
        const auto* velocity = reinterpret_cast<const JASBNKParser::Ver1::TVeloData*>(data);
        a.byte(velocity->_0, "velocity"), a.word(velocity->_4, "velocity wave"), a.word(velocity->mVolume, "velocity volume"),
            a.word(velocity->mPitch, "velocity pitch");
        a.plausible(std::isfinite(velocity->mVolume) && velocity->mVolume >= 0 && velocity->mVolume < 16, "a velocity region's volume");
        a.plausible(std::isfinite(velocity->mPitch) && velocity->mPitch > 0 && velocity->mPitch < 64, "a velocity region's pitch");
    }
}

void bankEffect(Audit& a, const std::uint8_t* bank, std::uint32_t offset) {
    const std::uint32_t* data = in<std::uint32_t>(bank, offset);
    a.word(*data, "effect kind");
    if (*data == tag("Rand")) {
        const auto* rand = reinterpret_cast<const JASBNKParser::Ver1::TRandData*>(data + 1);
        a.byte(rand->mType, "random target"), a.word(rand->mBase, "random base"), a.word(rand->mRange, "random range");
    } else if (*data == tag("Sens")) {
        const auto* sense = reinterpret_cast<const JASBNKParser::Ver1::TSenseData*>(data + 1);
        a.byte(sense->mType, "sense target"), a.byte(sense->mSenseType, "sense type"), a.byte(sense->mMaxPoint, "sense point");
        a.word(sense->mStartLvl, "sense start"), a.word(sense->mEndLvl, "sense end");
    } else {
        a.plausible(false, "an effect's kind");
    }
    ++a.counts["effects"];
}

void instrumentBank(Audit& a, const std::uint8_t* bank) {
    namespace V = JASBNKParser::Ver1;
    const auto* header = reinterpret_cast<const JASBNKParser::TFileHeader*>(bank);
    a.word(header->mSize, "IBNK size"), a.word(header->mVersion, "IBNK version");
    a.same(JASBNKParser::getBankNumber(bank), be32(a.before + a.at(bank) + 8), "IBNK bank number");
    const V::TChunk *envt = nullptr, *osct = nullptr, *list = nullptr;
    for (const auto* chunk = in<V::TChunk>(bank, 0x20); a.at(chunk) < a.at(bank) + header->mSize;
         chunk = in<V::TChunk>(bank, ((a.at(chunk) - a.at(bank)) + 0xB + chunk->mSize) & ~3u)) {
        a.word(chunk->mID, "chunk id"), a.word(chunk->mSize, "chunk size");
        if (chunk->mID == tag("ENVT")) envt = chunk;
        if (chunk->mID == tag("OSCT")) osct = chunk;
        if (chunk->mID == tag("LIST")) list = chunk;
    }
    a.plausible(envt && osct && list, "a bank's chunks");
    if (!envt || !osct || !list) return;
    const auto* points = reinterpret_cast<const std::int16_t*>(static_cast<const V::TEnvtChunk*>(envt)->mData);
    for (std::uint32_t i = 0; i < envt->mSize / 2; ++i) a.half(points[i], "envelope point");
    const auto* oscs = static_cast<const V::TOscChunk*>(osct);
    a.word(oscs->mCount, "oscillators");
    for (std::uint32_t o = 0; o < oscs->mCount; ++o) {
        const V::TOsc& osc = oscs->mOsc[o];
        a.byte(osc.mTarget, "oscillator target"), a.word(osc._08, "oscillator rate"), a.word(osc.mTableOffset, "oscillator table"),
            a.word(osc._10, "oscillator release table"), a.word(osc.mScale, "oscillator scale"), a.word(osc._18, "oscillator offset");
        a.plausible(osc.mTableOffset < envt->mSize && osc._10 < envt->mSize, "an oscillator's tables");
    }
    const auto* entries = static_cast<const V::TListChunk*>(list);
    a.word(entries->count, "instruments");
    for (std::uint32_t i = 0; i < entries->count; ++i) {
        a.word(entries->mOffsets[i], "instrument offset");
        if (!entries->mOffsets[i]) continue;
        const std::uint32_t* data = in<std::uint32_t>(bank, entries->mOffsets[i]);
        a.word(*data, "instrument kind");
        if (*data == tag("Inst")) {
            ++data;
            const std::uint32_t oscillators = *data++;
            for (std::uint32_t j = 0; j < oscillators; ++j, ++data) a.word(*data, "instrument oscillator"), a.plausible(*data < oscs->mCount, "an oscillator index");
            a.word(data[-int(oscillators) - 1], "instrument oscillators");
            const std::uint32_t effects = *data;
            a.word(*data++, "instrument effects");
            for (std::uint32_t j = 0; j < effects; ++j) a.word(*data, "effect offset"), bankEffect(a, bank, *data++);
            const std::uint32_t keys = *data;
            a.word(*data++, "key regions");
            for (std::uint32_t k = 0; k < keys; ++k) {
                a.same(data[0] >> 24, a.before[a.at(data)], "key region's high key");
                const std::uint32_t count = data[1];
                a.word(data[1], "velocity regions");
                data += 2;
                bankVelocities(a, data, count);
            }
            const auto* inst = reinterpret_cast<const V::TInstData*>(data);
            a.word(inst->mVolume, "instrument volume"), a.word(inst->mPitch, "instrument pitch");
            ++a.counts["instruments"];
        } else if (*data == tag("Perc")) {
            ++data;
            const std::uint32_t count = *data++;
            a.word(data[-1], "percussion keys");
            for (std::uint32_t k = 0; k < count; ++k) {
                const std::uint32_t offset = *data;
                a.word(*data++, "percussion key offset");
                if (!offset) continue;
                const std::uint32_t* ptr = in<std::uint32_t>(bank, offset);
                const auto* perc = reinterpret_cast<const V::TPercData*>(ptr + 1);
                a.word(perc->mVolume, "percussion volume"), a.word(perc->mPitch, "percussion pitch"), a.byte(perc->mPan, "percussion pan"),
                    a.half(perc->mRelease, "percussion release");
                ptr += 4;
                const std::uint32_t effects = *ptr;
                a.word(*ptr++, "percussion effects");
                for (std::uint32_t j = 0; j < effects; ++j) a.word(*ptr, "effect offset"), bankEffect(a, bank, *ptr++);
                const std::uint32_t velocities = *ptr;
                a.word(*ptr++, "percussion velocity regions");
                bankVelocities(a, ptr, velocities);
                ++a.counts["percussion keys"];
            }
        } else {
            a.plausible(false, "an instrument's kind");
        }
    }
}

void audioArchive(Audit& a, const Bytes& original) {
    Bytes copy = original;
    ++a.files;
    if (const char* why = smg::nativeAudioArchive(copy.data(), copy.size())) {
        ++a.refused;
        std::printf("%s: refused: %s\n", a.where.c_str(), why);
        return;
    }
    a.before = original.data(), a.after = copy.data();
    const std::uint8_t* d = original.data();
    a.plausible(std::memcmp(copy.data(), original.data(), 4) == 0, "the archive's signature (it stays big-endian)");
    for (std::size_t at = 4; at + 4 <= original.size();) {
        const std::uint32_t command = be32(d + at);
        auto argument = [&](unsigned n) { return be32(d + at + 4 + 4 * n); };
        unsigned arguments = 0;
        switch (command) {
        case tag("ws  "): waveSystem(a, copy.data() + argument(1)), ++a.counts["wave systems"], arguments = 3; break;
        case tag("bnk "): instrumentBank(a, copy.data() + argument(1)), ++a.counts["banks"], arguments = 2; break;
        case tag("bst "): soundTable(a, copy.data() + argument(0)), arguments = 2; break;
        case tag("bstn"): soundNames(a, copy.data() + argument(0)), arguments = 2; break;
        case tag("bsc "): sequenceCollection(a, copy.data() + argument(0)), arguments = 2; break;
        case tag("bl_<"): case tag("vbnk"): arguments = 2; break;
        case tag("bms "): arguments = 3; break;
        case tag("bmsa"): case tag("dsqb"): case tag("sect"): arguments = 1; break;
        case tag(">_bl"): break;
        case tag(">_AA"): return;
        default: a.plausible(false, "a command"); return;
        }
        a.plausible(std::memcmp(copy.data() + at, d + at, 4 + 4 * arguments) == 0, "a command (it stays big-endian)");
        at += 4 + 4 * arguments;
    }
}

Bytes readFile(const fs::path& path) {
    Bytes raw;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        raw.resize(fs::file_size(path));
        raw.resize(std::fread(raw.data(), 1, raw.size(), f));
        std::fclose(f);
    }
    return raw;
}

Bytes expand(const Bytes& raw) {
    if (raw.size() < 4 || std::memcmp(raw.data(), "Yaz0", 4) != 0) return raw;
    Bytes out;
    std::string error;
    if (!smg::decodeYaz0({raw.data(), raw.size()}, out, 64u << 20, error)) std::printf("Yaz0: %s\n", error.c_str());
    return out;
}

// The smaller files: each archive's members turn without a refusal, and their words read back.
void smallFiles(Audit& a, const fs::path& path, const char* (*convert)(std::uint8_t*, std::size_t), const char* suffix) {
    const Bytes archive = expand(readFile(path));
    std::vector<smg::ArchiveEntry> entries;
    std::string error;
    if (!smg::indexRarc({archive.data(), archive.size()}, entries, error)) {
        std::printf("%s: %s\n", path.c_str(), error.c_str());
        ++a.refused;
        return;
    }
    for (const auto& entry : entries) {
        if (entry.path.size() < 4 || entry.path.substr(entry.path.size() - 4) != suffix) continue;
        Bytes copy(archive.begin() + entry.offset, archive.begin() + entry.offset + entry.size);
        ++a.files;
        if (const char* why = convert(copy.data(), copy.size())) {
            ++a.refused;
            std::printf("%s %s: refused: %s\n", path.filename().c_str(), entry.path.c_str(), why);
        }
        ++a.counts[suffix];
    }
}
} // namespace

int auditAudio(const fs::path& root) {
    Audit a;
    a.where = "AudioRes/SMR.szs";
    audioArchive(a, expand(readFile(root / "AudioRes/SMR.szs")));
    smallFiles(a, root / "AudioRes/Info/JaiChord.arc", smg::nativeChordTable, ".cit");
    smallFiles(a, root / "AudioRes/Info/JaiMe.arc", smg::nativeMeTable, ".bmt");
    smallFiles(a, root / "AudioRes/Info/JaiMe.arc", smg::nativeMeSequences, ".bme");
    smallFiles(a, root / "AudioRes/Info/JaiRemixSeq.arc", smg::nativeRemixSequence, ".brs");
    smallFiles(a, root / "AudioRes/SpkRes/SpkRes.arc", smg::nativeSpeakerTable, ".bct");
    smallFiles(a, root / "AudioRes/SpkRes/SpkRes.arc", smg::nativeSpeakerWaves, ".csw");
    for (const auto& [what, count] : a.counts) std::printf("  %s: %zu\n", what.c_str(), count);
    std::printf("%zu files, %zu refused, %zu fields compared, %zu differ, %zu implausible\n", a.files, a.refused, a.fields, a.wrong,
                a.implausible);
    return a.refused || a.wrong || a.implausible ? 1 : 0;
}
