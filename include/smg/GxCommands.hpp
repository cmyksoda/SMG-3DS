#pragma once
#include <cstdint>

namespace smg {
// One vertex as the GPU reads it, dequantized: the attributes the CP's descriptor names, others left zero.
struct GxVertex {
    std::uint8_t matrixIndex = 0, texMatrixIndex[8] = {};
    float position[3] = {}, normal[3] = {}, texcoord[8][2] = {};
    std::uint8_t color[2][4] = {};
};

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
        // A whole primitive: its kind (the opcode's top bits: 0x80 quads ... 0xB8 points), vertex format and vertices.
        void (*primitive)(std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices);
        // BP 0x52: the pixel engine copies the EFB out (to the display, or to a texture), its settings in bp[0x49...0x52].
        void (*copy)(std::uint32_t command);
    };
    explicit GxCommandProcessor(const Host& host) : host_(host) {}

    // Bytes in the FIFO's order. Each stream (a thread, for the port) assembles its own commands, so two writers'
    // bytes never mix inside a command; whole commands reach the GPU's state in the order they complete.
    static constexpr unsigned Streams = 4;
    void put(unsigned stream, const std::uint8_t* bytes, std::uint32_t count);
    void put(const std::uint8_t* bytes, std::uint32_t count) { put(0, bytes, count); }
    // Bytes one vertex takes in a vertex format, by the CP's descriptor (VCD) and attribute table (VAT).
    std::uint32_t vertexSize(std::uint8_t format) const;
    // One vertex from `at`; indexed attributes are read from the CP's arrays through the host's memory.
    GxVertex readVertex(const std::uint8_t* at, std::uint8_t format) const;
    std::uint32_t pending(unsigned stream = 0) const { return pendingSize_[stream]; }

    std::uint32_t cp[0x100] = {};
    std::uint32_t xf[0x1100] = {};
    std::uint32_t bp[0x100] = {};
    // BP 0xE0-0xE7 hold a TEV colour register or a konst colour by their type bit: the last write of each kind.
    std::uint32_t tevColor[8] = {}, tevKonst[8] = {};

private:
    // One command from `at` if all of it is there: its length, or 0 to wait for more bytes.
    std::uint32_t command(const std::uint8_t* at, std::uint32_t available);
    void run(const std::uint8_t* at, std::uint32_t size);
    void loadBp(std::uint32_t value);

    Host host_;
    std::uint32_t bpMask_ = 0xFFFFFF;  // BP 0xFE: the bits the next BP load changes
    std::uint8_t pending_[Streams][0x10000];
    std::uint32_t pendingSize_[Streams] = {};
};
} // namespace smg
