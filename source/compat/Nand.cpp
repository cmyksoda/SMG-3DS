// The Wii's NAND as files in sdmc:/3ds/SMG-3DS/nand; the game's home is its title's data folder there, named by the
// disc's game code, and paths without a leading slash are the home's, as NANDInit leaves the current directory.
#include <3ds.h>
#include <revolution/nand.h>
#include <smg/DiscImage.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

namespace {
constexpr char sRoot[] = "sdmc:/3ds/SMG-3DS/nand";
constexpr char sBootBin[] = "sdmc:/3ds/SMG-3DS/sys/boot.bin";
constexpr int MaxOpen = 16;

LightLock sLock = 1;
std::FILE* sOpen[MaxOpen] = {};

[[noreturn]] void stop(const char* what) {
    std::fprintf(stderr, "The NAND on the SD card: %s\n", what);
    std::abort();
}

// "/title/00010000/<game code in hex>/data", as the Wii's ES names a disc title's data directory.
const char* home() {
    static char path[NAND_MAX_PATH] = {};
    if (!path[0]) {
        char code[4];
        if (smg::disc::fromImage()) {
            std::memcpy(code, smg::disc::bootBin(), 4);
        } else {
            std::FILE* boot = std::fopen(sBootBin, "rb");
            if (!boot || std::fread(code, 1, 4, boot) != 4) stop("sys/boot.bin is missing, so the game's title isn't known");
            std::fclose(boot);
        }
        std::snprintf(path, sizeof path, "/title/00010000/%02x%02x%02x%02x/data", static_cast<unsigned char>(code[0]),
                      static_cast<unsigned char>(code[1]), static_cast<unsigned char>(code[2]), static_cast<unsigned char>(code[3]));
    }
    return path;
}

std::string onCard(const char* path) {
    if (!path || std::strlen(path) >= NAND_MAX_PATH) return {};
    return std::string(sRoot) + (path[0] == '/' ? path : std::string(home()) + "/" + path);
}

void makeParents(const std::string& file) {
    for (std::size_t at = std::strlen("sdmc:/"); (at = file.find('/', at + 1)) != std::string::npos;) mkdir(file.substr(0, at).c_str(), 0777);
}

bool exists(const std::string& file) {
    struct stat st;
    return stat(file.c_str(), &st) == 0;
}

std::FILE* fileOf(const NANDFileInfo* info) {
    return info && info->fileDescriptor >= 0 && info->fileDescriptor < MaxOpen ? sOpen[info->fileDescriptor] : nullptr;
}

struct Locked {
    Locked() { LightLock_Lock(&sLock); }
    ~Locked() { LightLock_Unlock(&sLock); }
};
} // namespace

// The app ending: a save file being written finishes first, and none starts after (source/compat/OsReset.cpp).
extern "C" void smgNandHold() { LightLock_Lock(&sLock); }

extern "C" {
s32 NANDGetHomeDir(char path[NAND_MAX_PATH]) {
    std::strcpy(path, home());
    return NAND_RESULT_OK;
}

// The SD card's free space is the 3DS's own business; the game is told there is room.
s32 NANDCheck(u32, u32, u32* answer) {
    if (answer) *answer = 0;
    return NAND_RESULT_OK;
}

s32 NANDCreate(const char* path, u8, u8) {
    const Locked lock;
    const std::string file = onCard(path);
    if (file.empty()) return NAND_RESULT_INVALID;
    if (exists(file)) return NAND_RESULT_EXISTS;
    makeParents(file);
    std::FILE* created = std::fopen(file.c_str(), "wb");
    if (!created) return NAND_RESULT_ACCESS;
    std::fclose(created);
    return NAND_RESULT_OK;
}

// Opening never truncates, as IOS's doesn't: a shorter write leaves the old file's tail.
s32 NANDOpen(const char* path, NANDFileInfo* info, u8 access) {
    const Locked lock;
    const std::string file = onCard(path);
    if (file.empty() || !info || access < NAND_ACCESS_READ || access > NAND_ACCESS_RW) return NAND_RESULT_INVALID;
    if (!exists(file)) return NAND_RESULT_NOEXISTS;
    int slot = 0;
    while (slot < MaxOpen && sOpen[slot]) ++slot;
    if (slot == MaxOpen) return NAND_RESULT_MAXFD;
    std::FILE* opened = std::fopen(file.c_str(), access == NAND_ACCESS_READ ? "rb" : "r+b");
    if (!opened) return NAND_RESULT_ACCESS;
    sOpen[slot] = opened;
    std::memset(info, 0, sizeof *info);
    info->fileDescriptor = slot;
    info->origFd = -1;
    info->accType = access;
    std::strncpy(info->origPath, path, NAND_MAX_PATH - 1);
    return NAND_RESULT_OK;
}

s32 NANDClose(NANDFileInfo* info) {
    const Locked lock;
    std::FILE* file = fileOf(info);
    if (!file) return NAND_RESULT_INVALID;
    const bool written = std::fclose(file) == 0;
    sOpen[info->fileDescriptor] = nullptr;
    info->fileDescriptor = -1;
    return written ? NAND_RESULT_OK : NAND_RESULT_ACCESS;
}

s32 NANDRead(NANDFileInfo* info, void* buffer, u32 length) {
    const Locked lock;
    std::FILE* file = fileOf(info);
    if (!file || !(info->accType & NAND_ACCESS_READ)) return NAND_RESULT_ACCESS;
    return static_cast<s32>(std::fread(buffer, 1, length, file));
}

s32 NANDWrite(NANDFileInfo* info, const void* buffer, u32 length) {
    const Locked lock;
    std::FILE* file = fileOf(info);
    if (!file || !(info->accType & NAND_ACCESS_WRITE)) return NAND_RESULT_ACCESS;
    const std::size_t written = std::fwrite(buffer, 1, length, file);
    return written == length ? static_cast<s32>(written) : NAND_RESULT_MAXBLOCKS;
}

s32 NANDGetLength(NANDFileInfo* info, u32* length) {
    const Locked lock;
    std::FILE* file = fileOf(info);
    if (!file || !length) return NAND_RESULT_INVALID;
    const long at = std::ftell(file);
    std::fseek(file, 0, SEEK_END);
    *length = static_cast<u32>(std::ftell(file));
    std::fseek(file, at, SEEK_SET);
    return NAND_RESULT_OK;
}

s32 NANDDelete(const char* path) {
    const Locked lock;
    const std::string file = onCard(path);
    if (file.empty()) return NAND_RESULT_INVALID;
    if (!exists(file)) return NAND_RESULT_NOEXISTS;
    return std::remove(file.c_str()) == 0 ? NAND_RESULT_OK : NAND_RESULT_ACCESS;
}

// Moves a file into a directory under its own name, replacing one already there.
s32 NANDMove(const char* path, const char* destDir) {
    const Locked lock;
    const std::string from = onCard(path), dir = onCard(destDir);
    if (from.empty() || dir.empty()) return NAND_RESULT_INVALID;
    if (!exists(from)) return NAND_RESULT_NOEXISTS;
    const char* slash = std::strrchr(path, '/');
    const std::string to = dir + "/" + (slash ? slash + 1 : path);
    makeParents(to);
    if (exists(to)) std::remove(to.c_str());
    return std::rename(from.c_str(), to.c_str()) == 0 ? NAND_RESULT_OK : NAND_RESULT_ACCESS;
}

// The banner the Wii Menu shows beside a save: the SDK's own fill (its layout is the NAND's file format).
void NANDInitBanner(NANDBanner* banner, u32 flag, const u16* title, const u16* comment) {
    std::memset(banner, 0, sizeof *banner);
    banner->signature = NAND_BANNER_SIGNATURE;
    banner->flag = flag;
    const u16* texts[2] = {title, comment};
    for (int k = 0; k < 2; ++k) {
        if (!texts[k] || !texts[k][0]) banner->comment[k][0] = ' ';  // an empty line is a space
        for (int i = 0; texts[k] && i < NAND_BANNER_COMMENT_SIZE && texts[k][i]; ++i) banner->comment[k][i] = texts[k][i];
    }
}
}
