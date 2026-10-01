// Big-endian game data turned to native order in place, format by format, where original code first reads it.
// See compat/smg/NativeResources.hpp.
#include <JSystem/JKernel/JKRArchive.hpp>
#include <smg/AudioData.hpp>
#include <smg/Bmg.hpp>
#include <smg/FreeStore.hpp>
#include <smg/J3D.hpp>
#include <smg/Jpa.hpp>
#include <smg/Kcl.hpp>
#include <smg/Layout.hpp>
#include <smg/NativeResources.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace {
void swap(u32& value) { value = __builtin_bswap32(value); }
void swap(u16& value) { value = __builtin_bswap16(value); }
void swapWords(void* start, int count) {
    for (u32* word = static_cast<u32*>(start); count-- > 0; ++word) swap(*word);
}
constexpr u32 sRarc = 0x52415243;  // 'RARC' as a native word
} // namespace

// A RARC's header, info block, directories and file entries; the files' own data stays as the archive holds it,
// for its own format to turn.
void smgNativeRarc(void* archive) {
    auto* header = static_cast<JKRArchive::RarcHeader*>(archive);
    if (header->mMagic != __builtin_bswap32(sRarc)) return;
    swapWords(header, sizeof *header / 4);
    auto* info = reinterpret_cast<JKRArchive::RarcInfoBlock*>(static_cast<u8*>(archive) + header->mHeaderSize);
    swapWords(info, 6);
    swap(info->mNextAvailableFileID);
    swap(info->mFileIDIsIndex);
    auto* directories = reinterpret_cast<JKRArchive::SDIDirEntry*>(reinterpret_cast<u8*>(info) + info->mDirOffset);
    for (u32 i = 0; i < info->mNrDirs; ++i) {
        swap(directories[i].mID);
        swap(directories[i].mNameOffset);
        swap(directories[i].mHash);
        swap(directories[i].mNrFiles);
        swap(directories[i].mFirstFileIndex);
    }
    auto* files = reinterpret_cast<JKRArchive::SDIFileEntry*>(reinterpret_cast<u8*>(info) + info->mFileOffset);
    for (u32 i = 0; i < info->mNrFiles; ++i) {
        swap(files[i].mFileID);
        swap(files[i].mHash);
        swapWords(reinterpret_cast<u8*>(&files[i]) + 4, 3);  // flag and name offset, data offset or directory, size
    }
    // BTI textures' headers, KCL collision and JParticle containers turn here, where every reader's archive passes (texels stay the GPU's).
    const char* names = reinterpret_cast<const char*>(info) + info->mStringTableOffset;
    u8* data = static_cast<u8*>(archive) + header->mHeaderSize + header->mFileDataOffset;
    for (u32 i = 0; i < info->mNrFiles; ++i) {
        if (files[i].mFlag & 2) continue;  // a directory
        const char* name = names + files[i].mNameOffset;
        const char* dot = std::strrchr(name, '.');
        const char* (*turn)(std::uint8_t*, std::size_t) = nullptr;
        if (dot && !strcasecmp(dot, ".bti")) turn = smg::nativeBti;
        if (dot && !strcasecmp(dot, ".kcl")) turn = smg::nativeKcl;
        if (dot && !strcasecmp(dot, ".jpc")) turn = smg::nativeJpc;
        if (!turn) continue;
        if (const char* why = turn(data + files[i].mDataOffset, files[i].mDataSize)) {
            std::fprintf(stderr, "An archive's %s: %s\n", name, why);
            std::abort();
        }
    }
}

void smgNativeLayoutFile(const char* name, void* data, u32 size) {
    const smg::PortMemory memory;
    const char* dot = std::strrchr(name, '.');
    const char* (*turn)(std::uint8_t*, std::size_t) = nullptr;
    if (dot && !strcasecmp(dot, ".brlyt")) turn = smg::nativeBrlyt;
    if (dot && !strcasecmp(dot, ".brlan")) turn = smg::nativeBrlan;
    if (dot && !strcasecmp(dot, ".tpl")) turn = smg::nativeTpl;
    if (!turn) return;
    if (const char* why = turn(static_cast<std::uint8_t*>(data), size)) {
        std::fprintf(stderr, "A layout archive's %s %s\n", name, why);
        std::abort();
    }
}

void smgNativeFont(void* brfnt) {
    const smg::PortMemory memory;
    auto* bytes = static_cast<std::uint8_t*>(brfnt);
    if (bytes == nullptr || std::memcmp(bytes, "RFNT", 4) != 0) return;  // already native, or not a font (ResFont says so)
    const u32 size = u32(bytes[8]) << 24 | u32(bytes[9]) << 16 | u32(bytes[10]) << 8 | bytes[11];
    if (const char* why = smg::nativeBrfnt(bytes, size)) {
        std::fprintf(stderr, "A font (BRFNT) %s\n", why);
        std::abort();
    }
}

void smgNativeBmg(void* bmg, u32 size) {
    const smg::PortMemory memory;
    if (const char* why = smg::nativeBmg(static_cast<std::uint8_t*>(bmg), size)) {
        std::fprintf(stderr, "A BMG message file %s\n", why);
        std::abort();
    }
}

void smgNativeMeTable(void* table, u32 size) {
    const smg::PortMemory memory;
    if (const char* why = smg::nativeMeTable(static_cast<std::uint8_t*>(table), size)) {
        std::fprintf(stderr, "The music-event table (.bmt) %s\n", why);
        std::abort();
    }
}

void smgNativeMeSequences(void* sequences, u32 size) {
    const smg::PortMemory memory;
    if (const char* why = smg::nativeMeSequences(static_cast<std::uint8_t*>(sequences), size)) {
        std::fprintf(stderr, "The music-event sequences (.bme) %s\n", why);
        std::abort();
    }
}

void smgNativeChordTable(void* table, u32 size) {
    const smg::PortMemory memory;
    if (const char* why = smg::nativeChordTable(static_cast<std::uint8_t*>(table), size)) {
        std::fprintf(stderr, "A chord table (.cit) %s\n", why);
        std::abort();
    }
}

void smgNativeRemixSequence(void* sequence, u32 size) {
    const smg::PortMemory memory;
    if (const char* why = smg::nativeRemixSequence(static_cast<std::uint8_t*>(sequence), size)) {
        std::fprintf(stderr, "The remix sequence (.brs) %s\n", why);
        std::abort();
    }
}

void smgNativeSpeakerTable(void* table, u32 size) {
    const smg::PortMemory memory;
    if (const char* why = smg::nativeSpeakerTable(static_cast<std::uint8_t*>(table), size)) {
        std::fprintf(stderr, "The Wii Remote speaker's table (.bct) %s\n", why);
        std::abort();
    }
}

void smgNativeSpeakerWaves(void* waves, u32 size) {
    const smg::PortMemory memory;
    if (const char* why = smg::nativeSpeakerWaves(static_cast<std::uint8_t*>(waves), size)) {
        std::fprintf(stderr, "The Wii Remote speaker's waves (.csw) %s\n", why);
        std::abort();
    }
}

void smgNativeAudioArchive(void* archive, s32 size) {
    const smg::PortMemory memory;
    const char* why = size <= 0 ? "is in no heap the port can bound it by"
                                : smg::nativeAudioArchive(static_cast<std::uint8_t*>(archive), static_cast<std::size_t>(size));
    if (why) {
        std::fprintf(stderr, "The audio archive (SMR.szs) %s\n", why);
        std::abort();
    }
}
