#pragma once
#include <smg/Archive.hpp>
#include <Game/Util/JMapInfo.hpp>

namespace smg {
// Owns an aligned native-endian image so original JMapInfo can read it directly.
class Bcsv {
public:
    Bcsv() = default;
    Bcsv(const Bcsv&) = delete;
    Bcsv& operator=(const Bcsv&) = delete;
    bool load(Bytes source, std::string& error);
    const JMapInfo& info() const { return info_; }
private:
    std::vector<std::uint32_t> native_;
    JMapInfo info_;
};
} // namespace smg
