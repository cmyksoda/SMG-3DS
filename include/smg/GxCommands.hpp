#pragma once
#include <cstdint>

namespace smg {
// One vertex as the GPU reads it, dequantized: the attributes the CP's descriptor names, others left zero.
struct GxVertex {
    std::uint8_t matrixIndex = 0, texMatrixIndex[8] = {};
    float position[3] = {}, normal[3] = {}, texcoord[8][2] = {};
    std::uint8_t color[2][4] = {};
};

class GxCommandProcessor;

// A vertex format's layout, read once from the CP's descriptor (VCD), attribute table (VAT) and array registers: what
// each vertex of a primitive is read by. It stays right while the processor's cpChanges stays the same.
class GxVertexLayout {
public:
    GxVertexLayout() = default;
    GxVertexLayout(const GxCommandProcessor& gpu, std::uint8_t format);
    // After a CP change: when the format's words are the ones it was read from, only its arrays' bases and strides
    // are read again (J3D sets the arrays before every shape) and this returns true.
    bool refresh(const GxCommandProcessor& gpu, std::uint8_t format);
    // Overwrites the attributes the format has; the others keep what `vertex` held.
    void read(const std::uint8_t* at, GxVertex& vertex) const;
    // The arrays (0 position, 1 normal, 2-3 colours, 4-11 texture coordinates) this format reads by index, a bit each.
    std::uint16_t indexedArrays() const;
    // Widens [lo, hi) over the array elements the vertex at `at` reads by index (what a cached copy depends on).
    void indexedSpan(const std::uint8_t* at, const std::uint8_t*& lo, const std::uint8_t*& hi) const;
    std::uint32_t size = 0;

private:
    // type: 0 none, 1 direct, 2 an 8-bit index, 3 a 16-bit one. count: components; skip: extra index bytes (NBT's three).
    // An indexed attribute's array starts at `array` in the host's memory; scale is 1 / 2^shift. `decode` reads its
    // components and moves past it, made for its kind, format and count (decodeAs) or general (decodeAny).
    struct Attribute;
    using Decoder = void (*)(const Attribute&, const std::uint8_t*&, float*);
    struct Attribute {
        std::uint8_t type = 0, format = 0, count = 0, shift = 0, skip = 0, componentSize = 0, directSize = 0, arrayIndex = 0;
        std::uint32_t base = 0, stride = 0;
        float scale = 1;
        const std::uint8_t* array = nullptr;
        Decoder decode = nullptr;
    };
    static const std::uint8_t* source(const Attribute& a, const std::uint8_t*& at);
    template <int Kind, int Format, int Count> static void decodeAs(const Attribute& a, const std::uint8_t*& at, float* out);
    static void decodeAny(const Attribute& a, const std::uint8_t*& at, float* out);
    static Decoder pick(const Attribute& a);
    bool matrixIndex_ = false;
    std::uint8_t texMatrixIndexes_ = 0;
    std::uint32_t words_[5] = {};  // VCD low and high, VAT A, B and C as read
    bool read_ = false;
    Attribute position_, normal_, color_[2], texcoord_[8];
    std::uint8_t texcoords_[8] = {}, texcoordCount_ = 0;  // the sets the format has, in order
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
        // A display list called (physical address, bytes): true when the host drew it whole, so it isn't read here.
        bool (*displayList)(std::uint32_t physical, std::uint32_t size) = nullptr;
        // The port's own command 0x01 (no GX opcode): texture copies from here on stay on the GPU (true) or not, in
        // the stream's order, as the copies they wrap are.
        void (*copiesStay)(bool stay) = nullptr;
    };
    explicit GxCommandProcessor(const Host& host) : host_(host) {}

    // Bytes in the FIFO's order. Each stream (a thread, for the port) assembles its own commands, so two writers'
    // bytes never mix inside a command; whole commands reach the GPU's state in the order they complete.
    static constexpr unsigned Streams = 4;
    void put(unsigned stream, const std::uint8_t* bytes, std::uint32_t count);
    void put(const std::uint8_t* bytes, std::uint32_t count) { put(0, bytes, count); }
    // Bytes one vertex takes in a vertex format, by the CP's descriptor (VCD) and attribute table (VAT).
    std::uint32_t vertexSize(std::uint8_t format) const { return layout(format).size; }
    // The format's layout, read again only after the CP's registers change.
    const GxVertexLayout& layout(std::uint8_t format) const;
    // Whether a draw reads BP register `address` (what bpChanges counts).
    static bool drawsRead(std::uint32_t address);
    // One vertex from `at`; indexed attributes are read from the CP's arrays through the host's memory.
    GxVertex readVertex(const std::uint8_t* at, std::uint8_t format) const;
    const std::uint8_t* (*memoryFunction() const)(std::uint32_t) { return host_.memory; }
    std::uint32_t pending(unsigned stream = 0) const { return pendingSize_[stream]; }

    std::uint32_t cp[0x100] = {};
    std::uint32_t xf[0x1100] = {};
    std::uint32_t bp[0x100] = {};
    // BP 0xE0-0xE7 hold a TEV colour register or a konst colour by their type bit: the last write of each kind.
    std::uint32_t tevColor[8] = {}, tevKonst[8] = {};
    // Loads that changed something, by kind: equal counts mean that part of the state is as it was. bpChanges counts only
    // the BP registers a draw reads (TEV, textures, pixel tests, scissor, culling), so a run of primitives can share one draw.
    std::uint32_t bpChanges = 0, xfChanges = 0, cpChanges = 0;
    // Loads that changed an XF register (0x1000 up) rather than matrix memory.
    std::uint32_t xfRegisterChanges = 0;
    // Changes to each four-word row of XF memory (the matrices, below 0x1000): what read a row still holds while its count stays.
    std::uint32_t xfRowChanges[0x400] = {};
    // The same for every register a draw reads but the TEV colours C0 and C1, which can ride in the vertices and a
    // palette instead; C1's own changes apart.
    std::uint32_t bpChangesBesideColors = 0, color1Changes = 0;
    // Display lists called, and their bytes (for the renderer's perf line).
    std::uint32_t displayListCalls = 0, displayListBytes = 0;
    // How deep the processor is in called display lists (0: the FIFO itself).
    unsigned displayListDepth = 0;

private:
    // One command from `at` if all of it is there: its length, or 0 to wait for more bytes.
    std::uint32_t command(const std::uint8_t* at, std::uint32_t available);
    void run(const std::uint8_t* at, std::uint32_t size);
    void loadBp(std::uint32_t value);
    void loadXf(std::uint32_t address, std::uint32_t count, const std::uint8_t* from, bool native);

    Host host_;
    mutable GxVertexLayout layouts_[8];
    mutable std::uint32_t layoutChanges_[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    std::uint32_t bpMask_ = 0xFFFFFF;  // BP 0xFE: the bits the next BP load changes
    // The whole length of the command a stream has begun, from its header (at least the header itself).
    std::uint32_t length(const std::uint8_t* at, std::uint32_t available) const;
    std::uint8_t pending_[Streams][0x10000];
    std::uint32_t pendingSize_[Streams] = {};
};
} // namespace smg
