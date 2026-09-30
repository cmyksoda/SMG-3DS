// Big-endian game data turned to native order in place, format by format, where original code first reads it.
// See compat/smg/NativeResources.hpp.
#include <JSystem/JKernel/JKRArchive.hpp>
#include <smg/Bmg.hpp>
#include <smg/NativeResources.hpp>
#include <cstdio>
#include <cstdlib>

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
}

void smgNativeBmg(void* bmg, u32 size) {
    if (const char* why = smg::nativeBmg(static_cast<std::uint8_t*>(bmg), size)) {
        std::fprintf(stderr, "A BMG message file %s\n", why);
        std::abort();
    }
}
