#include <smg/GxCommands.hpp>
#include <cstring>

namespace smg {
namespace {
std::uint32_t word(const std::uint8_t* at) { return std::uint32_t(at[0]) << 24 | std::uint32_t(at[1]) << 16 | std::uint32_t(at[2]) << 8 | at[3]; }
std::uint32_t half(const std::uint8_t* at) { return std::uint32_t(at[0]) << 8 | at[1]; }
// Arrays in memory are the CPU's (big-endian on the Wii, native here); commands and display lists stay the GPU's.
std::uint32_t half(const std::uint8_t* at, bool native) {
    std::uint16_t value;
    std::memcpy(&value, at, 2);
    return native ? value : half(at);
}
std::uint32_t word(const std::uint8_t* at, bool native) {
    std::uint32_t value;
    std::memcpy(&value, at, 4);
    return native ? value : word(at);
}
std::uint32_t field(std::uint32_t reg, int shift, int size) { return reg >> shift & ((1u << size) - 1); }

// The CP's registers for vertices, as private/cp_reg.h and gd/GDGeometry.h number them.
constexpr int VcdLo = 0x50, VcdHi = 0x60, VatA = 0x70, VatB = 0x80, VatC = 0x90, ArrayBase = 0xA0, ArrayStride = 0xB0;

// A component's bytes by its format: u8, s8, u16, s16, f32.
std::uint32_t componentSize(std::uint32_t format) { return format == 4 ? 4 : format >= 2 ? 2 : 1; }
float component(const std::uint8_t* at, std::uint32_t format, std::uint32_t shift, bool native) {
    float value;
    switch (format) {
    case 0: value = at[0]; break;
    case 1: value = static_cast<std::int8_t>(at[0]); break;
    case 2: value = static_cast<float>(half(at, native)); break;
    case 3: value = static_cast<std::int16_t>(half(at, native)); break;
    default: {
        const std::uint32_t bits = word(at, native);
        std::memcpy(&value, &bits, 4);
        return value;
    }
    }
    return value / static_cast<float>(1u << shift);
}
// A colour's bytes by its format: RGB565, RGB888, RGB888x, RGBA4444, RGBA6666, RGBA8888.
std::uint32_t colorSize(std::uint32_t format) { static const std::uint8_t sizes[] = {2, 3, 4, 2, 3, 4, 4, 4}; return sizes[format & 7]; }
void readColor(const std::uint8_t* at, std::uint32_t format, bool native, std::uint8_t out[4]) {
    // A field of `bits` widened to eight, its top bits repeated below as the GPU widens it.
    const auto widen = [](std::uint32_t value, int bits) { return static_cast<std::uint8_t>(value << (8 - bits) | value >> (2 * bits - 8)); };
    if (format == 0) {  // RGB565
        const std::uint32_t v = half(at, native);
        out[0] = widen(v >> 11, 5);
        out[1] = widen(v >> 5 & 63, 6);
        out[2] = widen(v & 31, 5);
        out[3] = 255;
    } else if (format == 1 || format == 2) {  // RGB888, RGB888x
        std::memcpy(out, at, 3);
        out[3] = 255;
    } else if (format == 3) {  // RGBA4444
        const std::uint32_t v = half(at, native);
        for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>((v >> (12 - 4 * i) & 15) * 17);
    } else if (format == 4) {  // RGBA6666
        const std::uint32_t v = std::uint32_t(at[0]) << 16 | std::uint32_t(at[1]) << 8 | at[2];
        for (int i = 0; i < 4; ++i) out[i] = widen(v >> (18 - 6 * i) & 63, 6);
    } else {  // RGBA8888
        std::memcpy(out, at, 4);
    }
}
} // namespace

std::uint32_t GxCommandProcessor::vertexSize(std::uint8_t format) const {
    const std::uint32_t lo = cp[VcdLo], hi = cp[VcdHi], a = cp[VatA + format], b = cp[VatB + format], c = cp[VatC + format];
    std::uint32_t size = 0;
    for (int i = 0; i < 9; ++i) size += field(lo, i, 1);  // the position/normal matrix index and eight texture matrix indices
    const auto attribute = [&](std::uint32_t type, std::uint32_t direct, std::uint32_t indexes = 1) {
        return type == 0 ? 0 : type == 1 ? direct : type == 2 ? indexes : 2 * indexes;
    };
    size += attribute(field(lo, 9, 2), (field(a, 0, 1) + 2) * componentSize(field(a, 1, 3)));
    const bool nbt = field(a, 9, 1), threeIndexes = nbt && field(a, 31, 1);
    size += attribute(field(lo, 11, 2), (nbt ? 9 : 3) * componentSize(field(a, 10, 3)), threeIndexes ? 3 : 1);
    size += attribute(field(lo, 13, 2), colorSize(field(a, 14, 3)));
    size += attribute(field(lo, 15, 2), colorSize(field(a, 18, 3)));
    // Each coordinate set's count and format, from VAT A, B and C in turn.
    const std::uint32_t tex[8][2] = {{field(a, 21, 1), field(a, 22, 3)}, {field(b, 0, 1), field(b, 1, 3)}, {field(b, 9, 1), field(b, 10, 3)},
                                     {field(b, 18, 1), field(b, 19, 3)}, {field(b, 27, 1), field(b, 28, 3)}, {field(c, 5, 1), field(c, 6, 3)},
                                     {field(c, 14, 1), field(c, 15, 3)}, {field(c, 23, 1), field(c, 24, 3)}};
    for (int i = 0; i < 8; ++i) size += attribute(field(hi, 2 * i, 2), (tex[i][0] + 1) * componentSize(tex[i][1]));
    return size;
}

GxVertex GxCommandProcessor::readVertex(const std::uint8_t* at, std::uint8_t format) const {
    const std::uint32_t lo = cp[VcdLo], hi = cp[VcdHi], a = cp[VatA + format], b = cp[VatB + format], c = cp[VatC + format];
    GxVertex vertex;
    if (field(lo, 0, 1)) vertex.matrixIndex = *at++;
    for (int i = 0; i < 8; ++i) if (field(lo, 1 + i, 1)) vertex.texMatrixIndex[i] = *at++;
    // An attribute's own bytes: in the stream when direct, else in its array at the index the stream gives.
    bool native = false;
    const auto source = [&](std::uint32_t type, int array, std::uint32_t directSize) -> const std::uint8_t* {
        native = type != 1;
        if (type == 1) { const std::uint8_t* here = at; at += directSize; return here; }
        const std::uint32_t index = type == 2 ? *at : half(at);
        at += type == 2 ? 1 : 2;
        return host_.memory(cp[ArrayBase + array] + index * cp[ArrayStride + array]);
    };
    if (const std::uint32_t type = field(lo, 9, 2)) {
        const std::uint32_t count = field(a, 0, 1) + 2, fmt = field(a, 1, 3), size = componentSize(fmt);
        const std::uint8_t* data = source(type, 0, count * size);
        for (std::uint32_t i = 0; i < count; ++i) vertex.position[i] = component(data + i * size, fmt, field(a, 4, 5), native);
    }
    if (const std::uint32_t type = field(lo, 11, 2)) {
        // Normals take a fixed point by their format: six fraction bits for s8, fourteen for s16.
        const std::uint32_t fmt = field(a, 10, 3), size = componentSize(fmt), shift = fmt == 1 ? 6 : fmt == 3 ? 14 : 0;
        const bool nbt = field(a, 9, 1), threeIndexes = nbt && field(a, 31, 1);
        const std::uint8_t* data = source(type, 1, (nbt ? 9 : 3) * size);
        if (threeIndexes && type != 1) at += 2 * (type == 2 ? 1 : 2);  // binormal and tangent indexes, not kept
        for (int i = 0; i < 3; ++i) vertex.normal[i] = component(data + i * size, fmt, shift, native);
    }
    for (int channel = 0; channel < 2; ++channel) {
        if (const std::uint32_t type = field(lo, 13 + 2 * channel, 2)) {
            const std::uint32_t fmt = field(a, 14 + 4 * channel, 3);
            const std::uint8_t* data = source(type, 2 + channel, colorSize(fmt));
            readColor(data, fmt, native, vertex.color[channel]);
        }
    }
    const std::uint32_t tex[8][3] = {{field(a, 21, 1), field(a, 22, 3), field(a, 25, 5)}, {field(b, 0, 1), field(b, 1, 3), field(b, 4, 5)},
                                     {field(b, 9, 1), field(b, 10, 3), field(b, 13, 5)}, {field(b, 18, 1), field(b, 19, 3), field(b, 22, 5)},
                                     {field(b, 27, 1), field(b, 28, 3), field(c, 0, 5)}, {field(c, 5, 1), field(c, 6, 3), field(c, 9, 5)},
                                     {field(c, 14, 1), field(c, 15, 3), field(c, 18, 5)}, {field(c, 23, 1), field(c, 24, 3), field(c, 27, 5)}};
    for (int i = 0; i < 8; ++i) {
        if (const std::uint32_t type = field(hi, 2 * i, 2)) {
            const std::uint32_t count = tex[i][0] + 1, size = componentSize(tex[i][1]);
            const std::uint8_t* data = source(type, 4 + i, count * size);
            for (std::uint32_t k = 0; k < count; ++k) vertex.texcoord[i][k] = component(data + k * size, tex[i][1], tex[i][2], native);
        }
    }
    return vertex;
}

void GxCommandProcessor::loadBp(std::uint32_t value) {
    const std::uint32_t address = value >> 24;
    if (address == 0xFE) {  // the mask for the next load only
        bpMask_ = value & 0xFFFFFF;
        return;
    }
    const std::uint32_t data = (bp[address] & ~bpMask_) | (value & bpMask_);
    bpMask_ = 0xFFFFFF;
    bp[address] = data;
    if (address >= 0xE0 && address <= 0xE7) (data >> 23 & 1 ? tevKonst : tevColor)[address - 0xE0] = data;
    if (address == 0x45) host_.drawDone();                                    // PE_DONE
    else if (address == 0x47 || address == 0x48) host_.token(static_cast<std::uint16_t>(data), address == 0x48);  // PE_TOKEN(_INT)
    else if (address == 0x52 && host_.copy) host_.copy(data);
}

void GxCommandProcessor::run(const std::uint8_t* at, std::uint32_t size) {
    while (size) {
        const std::uint32_t used = command(at, size);
        if (!used) {
            host_.unsupported("a display list ends inside a command", size);
            return;
        }
        at += used;
        size -= used;
    }
}

std::uint32_t GxCommandProcessor::command(const std::uint8_t* at, std::uint32_t available) {
    const std::uint8_t opcode = at[0];
    if (opcode == 0x00 || opcode == 0x48) return 1;  // NOP; invalidate the vertex cache, which the port has none of
    if (opcode == 0x08) {                           // load a CP register
        if (available < 6) return 0;
        cp[at[1]] = word(at + 2);
        return 6;
    }
    if (opcode == 0x10) {  // load XF registers: the count less one, then the first address
        if (available < 5) return 0;
        const std::uint32_t header = word(at + 1), count = (header >> 16) + 1, address = header & 0xFFFF;
        if (available < 5 + count * 4) return 0;
        for (std::uint32_t i = 0; i < count; ++i)
            if (address + i < sizeof xf / sizeof *xf) xf[address + i] = word(at + 5 + i * 4);
        return 5 + count * 4;
    }
    if (opcode == 0x61) {  // load a BP register
        if (available < 5) return 0;
        loadBp(word(at + 1));
        return 5;
    }
    if (opcode == 0x40) {  // call a display list at its physical address
        if (available < 9) return 0;
        run(host_.memory(word(at + 1)), word(at + 5));
        return 9;
    }
    if (opcode >= 0x80 && opcode < 0xC0) {  // a primitive: its vertex count, then the vertices in its format
        if (available < 3) return 0;
        const std::uint16_t count = static_cast<std::uint16_t>(half(at + 1));
        const std::uint32_t length = 3 + count * vertexSize(opcode & 7);
        if (available < length) return 0;
        host_.primitive(opcode & 0xF8, opcode & 7, count, at + 3);
        return length;
    }
    if ((opcode & 0xE7) == 0x20) host_.unsupported("indexed XF loads are not the port's yet", opcode);
    else host_.unsupported("an opcode the Wii's GPU does not know", opcode);
    // Only reached when `unsupported` returns (a test's host): nothing past a command of unknown length can be read.
    return available;
}

void GxCommandProcessor::put(unsigned stream, const std::uint8_t* bytes, std::uint32_t count) {
    if (stream >= Streams) return host_.unsupported("a FIFO stream past the processor's", stream);
    std::uint8_t* pending = pending_[stream];
    std::uint32_t& size = pendingSize_[stream];
    if (size + count > sizeof pending_[stream]) {
        host_.unsupported("a command longer than the FIFO's staging", size + count);
        return;
    }
    std::memcpy(pending + size, bytes, count);
    size += count;
    std::uint32_t read = 0;
    while (read < size) {
        const std::uint32_t used = command(pending + read, size - read);
        if (!used) break;
        read += used;
    }
    std::memmove(pending, pending + read, size - read);
    size -= read;
}
} // namespace smg
