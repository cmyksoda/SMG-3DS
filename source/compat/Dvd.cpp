// The Wii's disc drive, over the game on the SD card: a disc image (DiscImage.cpp) or the files extracted from it.
// Entry numbers are the disc's own, from its file table (the image's, or sys/fst.bin); asynchronous reads finish on
// a worker thread, which calls the game back as the drive's interrupt did.
#include <3ds.h>
#include <revolution/dvd.h>
#include <smg/DiscImage.hpp>
#include <smg/FreeStore.hpp>
#include <smg/Interrupts.hpp>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr const char* sDiscRoot = "sdmc:/3ds/SMG-3DS/game";
constexpr const char* sFileTable = "sdmc:/3ds/SMG-3DS/sys/fst.bin";

// The SDK's sFatalError.
constexpr s32 sFatalError = -1;

[[noreturn]] void stop(const char* what) {
    std::fprintf(stderr, "The disc: %s\n", what);
    std::abort();
}

struct Entry {
    bool directory;
    smg::FreeString name, path;
    u32 size;       // a file's length
    u64 offset;     // a file's place in the disc's data partition
    u32 parent;     // a directory's parent
    u32 next;       // a directory's first entry past its contents
};
smg::FreeVector<Entry> sEntries;

u32 bigEndian(const u8* bytes) { return u32(bytes[0]) << 24 | u32(bytes[1]) << 16 | u32(bytes[2]) << 8 | bytes[3]; }

void readFileTable() {
    smg::FreeVector<u8> table;
    if (smg::disc::fromImage()) {
        const auto& fromImage = smg::disc::fileTable();
        table.assign(fromImage.begin(), fromImage.end());
    } else {
        FILE* file = std::fopen(sFileTable, "rb");
        if (!file) stop("neither a disc image (3ds/SMG-3DS/game.rvz, .wbfs or .iso) nor sys/fst.bin, the disc's file table, is on the SD card");
        std::fseek(file, 0, SEEK_END);
        table.resize(std::ftell(file));
        std::fseek(file, 0, SEEK_SET);
        const bool read = std::fread(table.data(), 1, table.size(), file) == table.size();
        std::fclose(file);
        if (!read) stop("sys/fst.bin could not be read");
    }
    if (table.size() < 12) stop("the disc's file table is too short");
    const u32 count = bigEndian(&table[8]);
    const u8* names = &table[12 * count];
    sEntries.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* raw = &table[12 * i];
        Entry& entry = sEntries[i];
        entry.directory = raw[0] != 0;
        entry.name = i == 0 ? "" : reinterpret_cast<const char*>(names + (bigEndian(raw) & 0xFFFFFF));
        entry.parent = entry.directory ? bigEndian(raw + 4) : 0;
        entry.next = entry.directory ? bigEndian(raw + 8) : i + 1;
        entry.size = entry.directory ? 0 : bigEndian(raw + 8);
        entry.offset = entry.directory ? 0 : u64(bigEndian(raw + 4)) << 2;
    }
    // Each entry's path, from the directory that holds it.
    for (u32 i = 0; i < count; ++i) {
        if (!sEntries[i].directory) continue;
        for (u32 child = i + 1; child < sEntries[i].next; child = sEntries[child].next) {
            sEntries[child].path = sEntries[i].path + "/" + sEntries[child].name;
        }
    }
}

// The SDK compares names without regard to case.
bool sameName(const char* a, size_t length, const smg::FreeString& b) {
    if (length != b.size()) return false;
    for (size_t i = 0; i < length; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

s32 find(const char* path) {
    u32 directory = 0;
    while (*path) {
        if (*path == '/') {
            ++path;
            continue;
        }
        const char* end = path;
        while (*end && *end != '/') ++end;
        const size_t length = end - path;
        if (length == 1 && path[0] == '.') {
        } else if (length == 2 && path[0] == '.' && path[1] == '.') {
            directory = sEntries[directory].parent;
        } else {
            if (!sEntries[directory].directory) return -1;
            u32 child = directory + 1;
            while (child < sEntries[directory].next && !sameName(path, length, sEntries[child].name)) child = sEntries[child].next;
            if (child >= sEntries[directory].next) return -1;
            directory = child;
        }
        path = end;
    }
    return static_cast<s32>(directory);
}

s32 readEntry(u32 entry, void* address, s32 length, s32 offset) {
    const Entry& file = sEntries[entry];
    if (offset < 0 || static_cast<u32>(offset) > file.size) return sFatalError;
    const u32 count = std::min<u32>(length, file.size - offset);
    if (smg::disc::fromImage()) return smg::disc::read(file.offset + offset, address, count) ? static_cast<s32>(count) : sFatalError;
    FILE* handle = std::fopen((sDiscRoot + file.path).c_str(), "rb");
    if (!handle) return sFatalError;
    std::fseek(handle, offset, SEEK_SET);
    const size_t read = std::fread(address, 1, count, handle);
    std::fclose(handle);
    return read == count ? static_cast<s32>(count) : sFatalError;
}

// Requests wait in the order they came, as the drive's queue kept them.
struct Request {
    DVDCommandBlock* block;
    DVDFileInfo* file;  // null for a disk check
    DVDCallback fileCallback;
    DVDCBCallback blockCallback;
};
smg::FreeVector<Request> sQueue;
LightLock sQueueLock = 1;
CondVar sQueueReady;
Thread sWorker;

void work(void*) {
    while (true) {
        LightLock_Lock(&sQueueLock);
        while (sQueue.empty()) CondVar_Wait(&sQueueReady, &sQueueLock);
        const Request request = sQueue.front();
        sQueue.erase(sQueue.begin());
        LightLock_Unlock(&sQueueLock);
        DVDCommandBlock* block = request.block;
        s32 result = 1;
        if (request.file) {
            result = readEntry(request.file->startAddr, block->addr, block->length, block->offset);
            block->transferredSize = result < 0 ? 0 : result;
        }
        block->state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
        const smg::AsInterrupt transferDone;
        if (request.fileCallback) request.fileCallback(result, request.file);
        if (request.blockCallback) request.blockCallback(result, block);
    }
}

void queue(const Request& request) {
    request.block->state = DVD_STATE_WAITING;
    LightLock_Lock(&sQueueLock);
    sQueue.push_back(request);
    CondVar_Signal(&sQueueReady);
    LightLock_Unlock(&sQueueLock);
}
} // namespace

extern "C" {
void DVDInit() {
    if (!sEntries.empty()) return;
    readFileTable();
    CondVar_Init(&sQueueReady);
    // Just ahead of the game's main thread (0x30), as the drive's interrupt was.
    sWorker = threadCreate(work, nullptr, 0x8000, 0x2F, -2, true);
    if (!sWorker) stop("no thread for the drive");
}

s32 DVDConvertPathToEntrynum(const char* path) { return find(path); }

BOOL DVDFastOpen(s32 entry, DVDFileInfo* file) {
    if (entry < 0 || static_cast<u32>(entry) >= sEntries.size() || sEntries[entry].directory) return FALSE;
    std::memset(file, 0, sizeof *file);
    file->startAddr = static_cast<u32>(entry);  // the disc offset on the Wii; here the entry is what a read needs
    file->length = sEntries[entry].size;
    file->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(const char* path, DVDFileInfo* file) { return DVDFastOpen(find(path), file); }
BOOL DVDClose(DVDFileInfo*) { return TRUE; }

s32 DVDReadPrio(DVDFileInfo* file, void* address, s32 length, s32 offset, s32) {
    const s32 result = readEntry(file->startAddr, address, length, offset);
    file->cb.transferredSize = result < 0 ? 0 : result;
    file->cb.state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
    return result;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* file, void* address, s32 length, s32 offset, DVDCallback callback, s32) {
    file->callback = callback;
    file->cb.addr = address;
    file->cb.length = length;
    file->cb.offset = offset;
    file->cb.transferredSize = 0;
    queue({&file->cb, file, callback, nullptr});
    return TRUE;
}

BOOL DVDCheckDiskAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    queue({block, nullptr, nullptr, callback});
    return TRUE;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) { return block->state; }

s32 DVDGetDriveStatus() {
    LightLock_Lock(&sQueueLock);
    const bool busy = !sQueue.empty();
    LightLock_Unlock(&sQueueLock);
    return busy ? DVD_STATE_BUSY : DVD_STATE_END;
}

// A request still waiting is taken out; one the worker has begun finishes, as a transfer under way did.
s32 DVDCancel(DVDCommandBlock* block) {
    LightLock_Lock(&sQueueLock);
    for (auto request = sQueue.begin(); request != sQueue.end(); ++request) {
        if (request->block == block) {
            sQueue.erase(request);
            block->state = DVD_STATE_CANCELED;
            break;
        }
    }
    LightLock_Unlock(&sQueueLock);
    return 0;
}

BOOL DVDOpenDir(const char* path, DVDDir* directory) {
    const s32 entry = find(path);
    if (entry < 0 || !sEntries[entry].directory) return FALSE;
    directory->entryNum = entry;
    directory->location = entry + 1;
    directory->next = sEntries[entry].next;
    return TRUE;
}

BOOL DVDReadDir(DVDDir* directory, DVDDirEntry* found) {
    if (directory->location >= directory->next) return FALSE;
    const Entry& entry = sEntries[directory->location];
    found->entryNum = directory->location;
    found->isDir = entry.directory;
    found->name = const_cast<char*>(entry.name.c_str());
    directory->location = entry.next;
    return TRUE;
}

BOOL DVDCloseDir(DVDDir*) { return TRUE; }
}
