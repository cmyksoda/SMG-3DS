// The game's disc read from an RVZ image, which keeps the partition decrypted: no key is needed. The file is read
// through the FS service, whose offsets are 64 bits (newlib's are 32, and an image runs past 2 GB).
#include <3ds.h>
#include <smg/DiscImage.hpp>
#include <smg/FreeStore.hpp>
#include <smg/WiiDisc.hpp>
#include <zstd.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
constexpr const char* sImagePath = "/3ds/SMG-3DS/game.rvz";

[[noreturn]] void stop(const char* what) {
    std::fprintf(stderr, "The disc image (3ds/SMG-3DS/game.rvz): %s\n", what);
    std::abort();
}

Handle sFile = 0;
ZSTD_DCtx* sZstd = nullptr;
smg::WiiDisc* sDisc = nullptr;
bool sTried = false;
LightLock sLock = 1;  // the drive's worker and the game's thread both read

smg::WiiDisc* opened() {
    if (sTried) return sDisc;
    sTried = true;
    if (R_FAILED(FSUSER_OpenFileDirectly(&sFile, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, sImagePath), FS_OPEN_READ, 0)))
        return nullptr;
    sZstd = ZSTD_createDCtx();
    if (!sZstd) stop("no memory to decompress it with");
    auto* disc = new smg::WiiDisc;
    disc->decompressWith([](const void* in, std::size_t size, void* out, std::size_t capacity) {
        const std::size_t made = ZSTD_decompressDCtx(sZstd, out, capacity, in, size);
        return ZSTD_isError(made) ? SIZE_MAX : made;
    });
    const auto reader = [](std::uint64_t offset, void* out, std::uint32_t size) {
        u32 read = 0;
        return R_SUCCEEDED(FSFILE_Read(sFile, &read, offset, out, size)) && read == size;
    };
    std::string error;
    if (!disc->open(reader, nullptr, nullptr, error)) stop(error.c_str());
    std::fprintf(stderr, "The disc: %s, game %s\n", disc->format(), disc->gameId().c_str());
    return sDisc = disc;
}

// The reader's buffers come from the free store, never the game's heaps.
struct Locked {
    smg::PortMemory memory;
    Locked() { LightLock_Lock(&sLock); }
    ~Locked() { LightLock_Unlock(&sLock); }
};
} // namespace

namespace smg::disc {
bool fromImage() {
    const Locked locked;
    return opened() != nullptr;
}

bool read(std::uint64_t offset, void* out, std::uint32_t size) {
    const Locked locked;
    return opened() && sDisc->read(offset, out, size);
}

const std::vector<std::uint8_t>& fileTable() {
    const Locked locked;
    if (!opened()) stop("asked for its file table with no image");
    return sDisc->fileTable();
}

const std::uint8_t* bootBin() {
    const Locked locked;
    if (!opened()) stop("asked for its boot.bin with no image");
    return sDisc->bootBin();
}
} // namespace smg::disc
