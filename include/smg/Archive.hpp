#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smg {
struct Bytes {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    bool contains(std::size_t offset, std::size_t count) const {
        return offset <= size && count <= size - offset;
    }
};
struct ArchiveEntry {
    std::string path;
    std::uint32_t offset, size;
    std::uint8_t flags;
};

// Wii archive fields are big endian and may be unaligned on ARM.
bool decodeYaz0(Bytes source, std::vector<std::uint8_t>& output, std::size_t memoryBudget, std::string& error);
bool indexRarc(Bytes source, std::vector<ArchiveEntry>& entries, std::string& error);
bool readArchive(const char* path, std::vector<std::uint8_t>& storage,
    std::vector<ArchiveEntry>& entries, std::size_t memoryBudget, std::string& error);
} // namespace smg
