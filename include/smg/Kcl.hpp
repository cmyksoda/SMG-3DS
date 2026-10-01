#pragma once
#include <smg/Archive.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace smg {
// A Wii KCL collision file, validated and repacked into aligned native-endian storage for the original
// KCollisionServer, as Bcsv does for JMapInfo. The header takes this build's KCLFile layout, so
// KCollisionServer::setData can turn its offsets into pointers of any width.
class Kcl {
public:
    Kcl() = default;
    Kcl(const Kcl&) = delete;
    Kcl& operator=(const Kcl&) = delete;
    bool load(Bytes source, std::string& error);
    void* data() { return storage_.data(); }
    std::size_t triangles() const { return triangles_; }
private:
    std::vector<std::uint64_t> storage_;
    std::size_t triangles_ = 0;
};
// The same file turned native in place, in the Wii's own layout (offsets the game turns into its 32-bit pointers):
// returns why it can't be, leaving it as it was, or null.
const char* nativeKcl(std::uint8_t* data, std::size_t size);
} // namespace smg
