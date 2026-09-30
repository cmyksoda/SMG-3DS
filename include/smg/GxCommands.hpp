#pragma once
#include <cstdint>

namespace smg {
// The Wii GPU's command processor: the FIFO's big-endian stream, taken byte by byte as GX sends it. It keeps the
// register loads, follows display lists and reports pixel-engine events; what it cannot take yet goes to `unsupported`.
class GxCommandProcessor {
public:
    struct Host {
        // A display list's physical address, as GX sends it, to where the CPU can read it.
        const std::uint8_t* (*memory)(std::uint32_t physical);
        void (*drawDone)();                 // BP 0x45
        void (*token)(std::uint16_t token, bool interrupt);  // BP 0x47 (no interrupt) and 0x48
        void (*unsupported)(const char* what, std::uint32_t detail);
    };
    explicit GxCommandProcessor(const Host& host) : host_(host) {}

    void put(const std::uint8_t* bytes, std::uint32_t count);
    std::uint32_t pending() const { return pendingSize_; }

    std::uint32_t cp[0x100] = {};
    std::uint32_t xf[0x1100] = {};
    std::uint32_t bp[0x100] = {};

private:
    // One command from `at` if all of it is there: its length, or 0 to wait for more bytes.
    std::uint32_t command(const std::uint8_t* at, std::uint32_t available);
    void run(const std::uint8_t* at, std::uint32_t size);
    void loadBp(std::uint32_t value);

    Host host_;
    std::uint8_t pending_[0x10000];
    std::uint32_t pendingSize_ = 0;
};
} // namespace smg
