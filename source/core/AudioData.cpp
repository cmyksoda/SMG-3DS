// The game's audio data turned native for the readers that overlay it with structs (include/smg/AudioData.hpp).
#include <smg/AudioData.hpp>
#include <smg/SwapPlan.hpp>
#include <utility>

namespace smg {
namespace {
std::uint32_t be32(const std::uint8_t* at) { return std::uint32_t(at[0]) << 24 | std::uint32_t(at[1]) << 16 | std::uint32_t(at[2]) << 8 | at[3]; }
void swap16(std::uint8_t* at) { std::swap(at[0], at[1]); }
void swap32(std::uint8_t* at) { std::swap(at[0], at[3]), std::swap(at[1], at[2]); }
bool fits(std::size_t at, std::size_t count, std::size_t size) { return at <= size && count <= size - at; }
} // namespace

const char* nativeMeTable(std::uint8_t* data, std::size_t size) {
    if (size < 12) return "is shorter than its header";
    const std::size_t count = be32(data), entries = be32(data + 4), names = be32(data + 8);
    if (!fits(entries, 8 * count, size) || !fits(names, 4 * count, size)) return "has entries or names past its end";
    for (std::size_t i = 0; i < count; ++i)
        if (be32(data + names + 4 * i) >= size) return "has a name past its end";
    for (int i = 0; i < 3; ++i) swap32(data + 4 * i);
    for (std::size_t i = 0; i < count; ++i) swap16(data + entries + 8 * i + 4);  // AudMePlayingParams::_4
    for (std::size_t i = 0; i < count; ++i) swap32(data + names + 4 * i);
    return nullptr;
}

const char* nativeMeSequences(std::uint8_t* data, std::size_t size) {
    if (size < 4) return "is shorter than its count";
    const std::size_t count = be32(data);
    if (!fits(4, 4 * count, size)) return "has more sequences than it holds";
    for (std::size_t i = 0; i < count; ++i)
        if (be32(data + 4 + 4 * i) >= size) return "has a sequence starting past its end";
    for (std::size_t i = 0; i <= count; ++i) swap32(data + 4 * i);
    return nullptr;
}

const char* nativeChordTable(std::uint8_t* data, std::size_t size) {
    if (size < 16 || data[4] != 'C' || data[5] != 'I' || data[6] != 'T' || data[7] != 'S') return "is not a chord table (CITS)";
    if (be32(data) != 0) return "was already relocated by the game";
    const std::size_t chords = std::size_t(data[12]) << 8 | data[13], scales = std::size_t(data[14]) << 8 | data[15];
    if (!fits(16, 4 * (chords + scales), size)) return "has more chords and scales than it holds";
    for (std::size_t i = 0; i < chords; ++i)
        if (!fits(be32(data + 16 + 4 * i), 8, size)) return "has a chord past its end";
    for (std::size_t i = 0; i < scales; ++i) {
        const std::size_t scale = be32(data + 16 + 4 * (chords + i));
        if (!fits(scale, 8, size) || scale % 4) return "has a scale past its end";
        if (be32(data + scale) >= size || be32(data + scale + 4) >= size) return "has a scale's notes past its end";
    }
    for (std::size_t i = 0; i < scales; ++i) {
        const std::size_t scale = be32(data + 16 + 4 * (chords + i));
        swap32(data + scale), swap32(data + scale + 4);
    }
    swap32(data), swap32(data + 8), swap16(data + 12), swap16(data + 14);
    for (std::size_t i = 0; i < chords + scales; ++i) swap32(data + 16 + 4 * i);
    return nullptr;
}

const char* nativeRemixSequence(std::uint8_t* data, std::size_t size) {
    // Walked as AudRemixMgr::setRemixSeqResource walks it, which skips the group offsets after the count.
    const std::size_t words = size / 4;
    auto word = [&](std::size_t at) { return be32(data + 4 * at); };
    if (words < 1) return "is shorter than its count";
    const std::size_t groups = word(0);
    std::size_t at = 1 + groups;
    for (std::size_t g = 0; g < groups; ++g) {
        if (at + 2 > words) return "has a group past its end";
        const std::size_t tracks = word(at), notes = word(at + 1);
        if (notes > words || tracks > words) return "has a group larger than the file";
        at += 2 + notes;
        for (std::size_t t = 0; t < tracks && at <= words; ++t) at += 1 + 4 * notes;
        if (at > words) return "has a group's tracks past its end";
    }
    for (std::size_t i = 0; i < words; ++i) swap32(data + 4 * i);
    return nullptr;
}

const char* nativeSpeakerTable(std::uint8_t* data, std::size_t size) {
    if (size < 16) return "is shorter than its header";
    if (be32(data + 12) != 0) return "was already relocated by the game";
    const std::size_t count = be32(data), entries = be32(data + 4), names = be32(data + 8);
    if (!fits(entries, 8 * count, size) || !fits(names, 4 * count, size)) return "has entries or names past its end";
    for (std::size_t i = 0; i < count; ++i)
        if (be32(data + names + 4 * i) >= size) return "has a name past its end";
    for (int i = 0; i < 4; ++i) swap32(data + 4 * i);
    for (std::size_t i = 0; i < count; ++i) swap16(data + entries + 8 * i), swap16(data + entries + 8 * i + 4), swap16(data + entries + 8 * i + 6);
    for (std::size_t i = 0; i < count; ++i) swap32(data + names + 4 * i);
    return nullptr;
}

const char* nativeSpeakerWaves(std::uint8_t* data, std::size_t size) {
    if (size < 8) return "is shorter than its header";
    const std::size_t count = be32(data + 4);
    if (!fits(8, 4 * count, size)) return "has more waves than it holds";
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t wave = be32(data + 8 + 4 * i);
        if (wave % 2 || !fits(wave, 12, size) || !fits(wave + 12, be32(data + wave), size) || be32(data + wave) % 2)
            return "has a wave past its end";
    }
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t* wave = data + be32(data + 8 + 4 * i);
        const std::size_t bytes = be32(wave);
        for (std::size_t at = 0; at < bytes; at += 2) swap16(wave + 12 + at);
        for (int w = 0; w < 3; ++w) swap32(wave + 4 * w);
    }
    for (std::size_t i = 0; i < 2 + count; ++i) swap32(data + 4 * i);
    return nullptr;
}

namespace {
constexpr std::uint32_t tag(const char (&text)[5]) {
    return std::uint32_t(std::uint8_t(text[0])) << 24 | std::uint32_t(std::uint8_t(text[1])) << 16 |
           std::uint32_t(std::uint8_t(text[2])) << 8 | std::uint8_t(text[3]);
}

// JAUSoundTable_'s tree, shared by BST and BSTN: the header's fourth word locates the root, then sections of groups
// of items, every offset from the table's start. Name tables' sections carry one more word before their groups.
template <typename Item>
void soundTree(SwapPlan& plan, std::size_t start, std::size_t end, bool names, Item item) {
    plan.words(start, 4);
    const std::size_t root = plan.from(start, plan.be32(start + 12));
    if (!plan.need(root, 4, end, "a root")) return;
    const std::uint32_t sections = plan.be32(root);
    if (!plan.need(root + 4, 4 * std::size_t(sections), end, "sections")) return;
    plan.words(root, 1 + sections);
    for (std::uint32_t s = 0; s < sections && plan.ok(); ++s) {
        if (plan.be32(root + 4 + 4 * s) == 0) continue;
        const std::size_t section = plan.from(start, plan.be32(root + 4 + 4 * s));
        const std::size_t head = names ? 8 : 4;
        if (!plan.need(section, head, end, "a section")) return;
        const std::uint32_t groups = plan.be32(section);
        if (!plan.need(section + head, 4 * std::size_t(groups), end, "a section's groups")) return;
        plan.words(section, head / 4 + groups);
        for (std::uint32_t g = 0; g < groups && plan.ok(); ++g) {
            if (plan.be32(section + head + 4 * g) == 0) continue;
            const std::size_t group = plan.from(start, plan.be32(section + head + 4 * g));
            if (!plan.need(group, 8, end, "a group")) return;
            const std::uint32_t items = plan.be32(group);
            if (!plan.need(group + 8, 4 * std::size_t(items), end, "a group's items")) return;
            plan.words(group, 2 + items);
            if (names && plan.be32(group + 4) != 0) plan.name(plan.from(start, plan.be32(group + 4)), end, "a group name");
            for (std::uint32_t i = 0; i < items && plan.ok(); ++i) item(plan.be32(group + 8 + 4 * i));
        }
    }
}

// BST: each item's word holds its type in the top byte (JAUSoundInfo's DATA_SE, DATA_BGM, DATA_STREAM) and its
// offset below; the item starts with its priority and volume bytes.
void soundTable(SwapPlan& plan, std::size_t start, std::size_t end) {
    if (!plan.is(start, "BST ")) return void(plan.fail("has a sound table that isn't BST"));
    soundTree(plan, start, end, false, [&](std::uint32_t word) {
        if ((word & 0xFFFFFF) == 0) return;
        const std::size_t item = plan.from(start, word & 0xFFFFFF);
        if (!plan.need(item, (word >> 24 & 0xF0) == 0x50 ? 6 : 8, end, "a sound")) return;
        plan.keep(item, 2);
        switch (word >> 24 & 0xF0) {
        case 0x50: plan.halves(item + 2, 2); break;  // JAUSoundTableSe
        case 0x60: plan.halves(item + 2, 3); break;  // JAUSoundTableBgm
        case 0x70:                                   // JAUSoundTableStream, its file name
            plan.half(item + 2), plan.word(item + 4);
            plan.name(plan.from(start, plan.be32(item + 4)), end, "a stream's file name");
            break;
        default: plan.fail("has a sound of type 0x%02x", unsigned(word >> 24));
        }
    });
}

void soundNames(SwapPlan& plan, std::size_t start, std::size_t end) {
    if (!plan.is(start, "BSTN")) return void(plan.fail("has a sound name table that isn't BSTN"));
    soundTree(plan, start, end, true, [&](std::uint32_t offset) {
        if (offset != 0) plan.name(plan.from(start, offset), end, "a sound name");
    });
}

// BSC (JAUSeqCollectionData): "SC", a group count, the file's size, then per group its sequences' offsets. The
// groups' tables sit at odd offsets, so they stay big-endian for getSeqData to read by bytes, like the sequences.
void sequenceCollection(SwapPlan& plan, std::size_t start, std::size_t end) {
    if (!plan.need(start, 8, end, "a sequence collection") || plan.be16(start) != 0x5343) return void(plan.fail("has a sequence collection that isn't SC"));
    const std::uint32_t groups = plan.be16(start + 2);
    if (!plan.need(start + 8, 4 * std::size_t(groups), end, "sequence groups")) return;
    plan.keep(start, 2), plan.half(start + 2), plan.word(start + 4), plan.words(start + 8, groups);
    for (std::uint32_t g = 0; g < groups && plan.ok(); ++g) {
        const std::size_t table = plan.from(start, plan.be32(start + 8 + 4 * g));
        if (!plan.need(table, 4, end, "a sequence group")) return;
        const std::uint32_t sequences = plan.be32(table);
        if (!plan.need(table + 4, 4 * std::size_t(sequences), end, "a sequence group's sequences")) return;
        plan.keep(table, 4 + 4 * std::size_t(sequences));
    }
}

// WSYS (JASWSParser): per group, a scene's control list of wave ids beside an archive's wave descriptions.
void waveSystem(SwapPlan& plan, std::size_t start, std::size_t end) {
    if (!plan.is(start, "WSYS")) return void(plan.fail("has a wave system that isn't WSYS"));
    if (!plan.need(start, 0x18, end, "a wave system header")) return;
    plan.keep(start, 4), plan.words(start + 4, 5);
    const std::size_t bank = plan.from(start, plan.be32(start + 0x10)), control = plan.from(start, plan.be32(start + 0x14));
    if (!plan.need(control, 0xC, end, "a control group") || !plan.need(bank, 8, end, "an archive bank")) return;
    const std::uint32_t groups = plan.be32(control + 8);
    if (!plan.need(control + 0xC, 4 * std::size_t(groups), end, "control scenes") ||
        !plan.need(bank + 8, 4 * std::size_t(groups), end, "wave archives"))
        return;
    plan.keep(control, 8), plan.words(control + 8, 1 + groups);
    plan.keep(bank, 4), plan.words(bank + 4, 1 + groups);
    for (std::uint32_t g = 0; g < groups && plan.ok(); ++g) {
        const std::size_t scene = plan.from(start, plan.be32(control + 0xC + 4 * g));
        if (!plan.need(scene, 0x10, end, "a control scene")) return;
        plan.keep(scene, 0xC), plan.word(scene + 0xC);
        const std::size_t ctrl = plan.from(start, plan.be32(scene + 0xC));
        if (!plan.need(ctrl, 8, end, "a control")) return;
        const std::uint32_t waves = plan.be32(ctrl + 4);
        if (!plan.need(ctrl + 8, 4 * std::size_t(waves), end, "a control's waves")) return;
        plan.keep(ctrl, 4), plan.words(ctrl + 4, 1 + waves);
        for (std::uint32_t w = 0; w < waves && plan.ok(); ++w) {
            const std::size_t id = plan.from(start, plan.be32(ctrl + 8 + 4 * w));
            if (plan.need(id, 4, end, "a wave id")) plan.word(id);  // JSULoHalf reads the wave's id as the word's low half
        }
        const std::size_t archive = plan.from(start, plan.be32(bank + 8 + 4 * g));
        if (!plan.need(archive, 0x74 + 4 * std::size_t(waves), end, "a wave archive")) return;
        plan.name(archive, archive + 0x70, "a wave archive's file name");
        plan.words(archive + 0x70, 1 + waves);
        for (std::uint32_t w = 0; w < waves && plan.ok(); ++w) {
            const std::size_t wave = plan.from(start, plan.be32(archive + 0x74 + 4 * w));
            if (!plan.need(wave, 0x24, end, "a wave")) return;
            plan.keep(wave, 4), plan.words(wave + 4, 7), plan.halves(wave + 0x20, 2);
        }
    }
}

// An instrument's effect (JASBNKParser::Ver1::createEffector): its kind, a type byte or three, then two floats.
void effect(SwapPlan& plan, std::size_t at, std::size_t end) {
    if (!plan.need(at, 0x10, end, "an effect")) return;
    const std::uint32_t kind = plan.be32(at);
    if (kind != tag("Rand") && kind != tag("Sens")) return void(plan.fail("has an effect of kind 0x%08lx", static_cast<unsigned long>(kind)));
    plan.word(at), plan.keep(at + 4, 4), plan.words(at + 8, 2);
}

// Effect offsets, then velocity regions: a byte, the wave id's word, volume and pitch.
std::size_t effectsAndVelocities(SwapPlan& plan, std::size_t start, std::size_t at, std::size_t end) {
    if (!plan.need(at, 4, end, "effects")) return end;
    const std::uint32_t effects = plan.be32(at);
    if (!plan.need(at + 4, 4 * std::size_t(effects) + 4, end, "effects")) return end;
    plan.words(at, 1 + effects);
    for (std::uint32_t e = 0; e < effects && plan.ok(); ++e) effect(plan, plan.from(start, plan.be32(at + 4 + 4 * e)), end);
    at += 4 + 4 * std::size_t(effects);
    return at;
}

std::size_t velocities(SwapPlan& plan, std::size_t at, std::uint32_t count, std::size_t end) {
    if (!plan.need(at, 0x10 * std::size_t(count), end, "velocity regions")) return end;
    for (std::uint32_t v = 0; v < count && plan.ok(); ++v) plan.keep(at + 0x10 * v, 4), plan.words(at + 0x10 * v + 4, 3);
    return at + 0x10 * std::size_t(count);
}

void instrument(SwapPlan& plan, std::size_t start, std::size_t at, std::size_t end) {
    if (!plan.need(at, 8, end, "an instrument")) return;
    const std::uint32_t oscillators = plan.be32(at + 4);
    if (!plan.need(at + 8, 4 * std::size_t(oscillators), end, "an instrument's oscillators")) return;
    plan.words(at, 2 + oscillators);
    at = effectsAndVelocities(plan, start, at + 8 + 4 * std::size_t(oscillators), end);
    if (!plan.need(at, 4, end, "key regions")) return;
    const std::uint32_t keys = plan.be32(at);
    plan.word(at), at += 4;
    for (std::uint32_t k = 0; k < keys && plan.ok(); ++k) {
        if (!plan.need(at, 8, end, "a key region")) return;
        const std::uint32_t count = plan.be32(at + 4);
        plan.words(at, 2);  // its high key is the first word's top byte
        at = velocities(plan, at + 8, count, end);
    }
    if (plan.need(at, 8, end, "an instrument's volume and pitch")) plan.words(at, 2);
}

void percussion(SwapPlan& plan, std::size_t start, std::size_t at, std::size_t end) {
    if (!plan.need(at, 8, end, "a percussion set")) return;
    const std::uint32_t count = plan.be32(at + 4);
    if (!plan.need(at + 8, 4 * std::size_t(count), end, "a percussion set's keys")) return;
    plan.words(at, 2 + count);
    for (std::uint32_t k = 0; k < count && plan.ok(); ++k) {
        if (plan.be32(at + 8 + 4 * k) == 0) continue;
        const std::size_t key = plan.from(start, plan.be32(at + 8 + 4 * k));
        if (!plan.need(key, 0x14, end, "a percussion key")) return;
        // Its kind, volume and pitch, the pan byte, the release half, then (as TPercData's last word) its effects.
        plan.words(key, 3), plan.keep(key + 0xC, 2), plan.half(key + 0xE);
        std::size_t next = effectsAndVelocities(plan, start, key + 0x10, end);
        if (!plan.need(next, 4, end, "a percussion key's velocities")) return;
        const std::uint32_t regions = plan.be32(next);
        plan.word(next);
        velocities(plan, next + 4, regions, end);
    }
}

// IBNK version 1 (JASBNKParser::Ver1): chunks found by walking from 0x20 to the header's size, each padded to a word
// in memory; the walk stops, as findChunk's do, once ENVT, OSCT and LIST are found. LIST's data often runs a word or
// two past that size (never into the next file), so chunk data is bounded by the archive, as the parser reads it.
void instrumentBank(SwapPlan& plan, std::size_t start, std::size_t end) {
    if (!plan.is(start, "IBNK") || !plan.need(start, 0x20, end, "a bank header")) return void(plan.fail("has a bank that isn't IBNK"));
    if (start % 4) return void(plan.fail("has a bank at 0x%zx its chunk walk would misalign", start));
    if (plan.be32(start + 12) != 1) return void(plan.fail("has a version %lu bank", static_cast<unsigned long>(plan.be32(start + 12))));
    const std::size_t walked = plan.from(start, plan.be32(start + 4));
    plan.words(start, 4);
    std::size_t list = 0, envelopes = 0, oscillators = 0;
    for (std::size_t chunk = start + 0x20; chunk < walked && plan.ok() && !(envelopes && oscillators && list);) {
        if (!plan.need(chunk, 8, end, "a bank chunk")) return;
        const std::uint32_t id = plan.be32(chunk), size = plan.be32(chunk + 4);
        plan.words(chunk, 2);
        if (!plan.need(chunk + 8, size, end, "a bank chunk's data")) return;
        if (id == tag("ENVT")) envelopes = chunk;
        if (id == tag("OSCT")) oscillators = chunk;
        if (id == tag("LIST")) list = chunk;
        if (id == tag("ENVT")) plan.halves(chunk + 8, size / 2);  // JASOscillator::Point, three s16 each
        chunk = (chunk + 0xB + size) & ~std::size_t(3);
    }
    if (!plan.ok()) return;
    if (!envelopes || !oscillators || !list) return void(plan.fail("has a bank without its ENVT, OSCT and LIST"));
    const std::uint32_t count = plan.be32(oscillators + 8);
    if (!plan.need(oscillators + 0xC, 0x1C * std::size_t(count), end, "oscillators")) return;
    plan.word(oscillators + 8);
    for (std::uint32_t o = 0; o < count; ++o) {
        const std::size_t osc = oscillators + 0xC + 0x1C * o;
        plan.word(osc), plan.keep(osc + 4, 4), plan.words(osc + 8, 5);
    }
    const std::uint32_t entries = plan.be32(list + 8);
    if (!plan.need(list + 0xC, 4 * std::size_t(entries), end, "a bank's list")) return;
    plan.words(list + 8, 1 + entries);
    for (std::uint32_t i = 0; i < entries && plan.ok(); ++i) {
        if (plan.be32(list + 0xC + 4 * i) == 0) continue;
        const std::size_t at = plan.from(start, plan.be32(list + 0xC + 4 * i));
        if (!plan.need(at, 4, end, "an instrument")) return;
        if (plan.be32(at) == tag("Inst")) instrument(plan, start, at, end);
        else if (plan.be32(at) == tag("Perc")) percussion(plan, start, at, end);
        else plan.fail("has an instrument of kind 0x%08lx", static_cast<unsigned long>(plan.be32(at)));
    }
}
} // namespace

const char* nativeAudioArchive(std::uint8_t* data, std::size_t size) {
    SwapPlan plan(data, size);
    if (!plan.is(0, "AA_<")) return "is not an audio archive (AA_<)";
    plan.keep(0, 4);
    for (std::size_t at = 4; plan.ok();) {
        if (!plan.need(at, 4, size, "a command")) break;
        const std::uint32_t command = plan.be32(at);
        auto argument = [&](unsigned n) { return plan.be32(at + 4 + 4 * n); };
        unsigned arguments = 0;
        switch (command) {
        case tag(">_AA"): break;
        case tag("ws  "): arguments = 3; break;
        case tag("bnk "): case tag("bl_<"): case tag("bsc "): case tag("bst "): case tag("bstn"): case tag("vbnk"): arguments = 2; break;
        case tag(">_bl"): break;
        case tag("bms "): arguments = 3; break;
        case tag("bmsa"): case tag("dsqb"): case tag("sect"): arguments = 1; break;
        case tag("bsft"): plan.fail("has a stream file table (bsft), which the port doesn't turn yet"); break;
        default: plan.fail("has a command 0x%08lx the interpreter doesn't know", static_cast<unsigned long>(command));
        }
        if (!plan.need(at, 4 + 4 * arguments, size, "a command's arguments")) break;
        plan.keep(at, 4 + 4 * arguments);
        switch (command) {
        case tag("ws  "): waveSystem(plan, argument(1), size); break;
        case tag("bnk "): instrumentBank(plan, argument(1), size); break;
        case tag("bsc "): sequenceCollection(plan, argument(0), plan.from(0, argument(1))); break;
        case tag("bst "): soundTable(plan, argument(0), plan.from(0, argument(1))); break;
        case tag("bstn"): soundNames(plan, argument(0), plan.from(0, argument(1))); break;
        }
        if (command == tag(">_AA")) return plan.apply();
        at += 4 + 4 * arguments;
    }
    return plan.apply();
}
} // namespace smg