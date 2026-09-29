#pragma once
#include <Game/System/NerveExecutor.hpp>
#include <string>

namespace smg {
class Session : public NerveExecutor {
public:
    explicit Session(const char* dataRoot = nullptr);
    void exeBoot();
    void exeWaiting();
    bool booted() const { return booted_; }
    const std::string& assetStatus() const { return assetStatus_; }
private:
    bool booted_ = false;
    std::string dataRoot_;
    std::string assetStatus_ = "Game data has not been checked";
};
} // namespace smg
