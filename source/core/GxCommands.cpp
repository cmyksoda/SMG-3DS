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

// The BP registers a draw reads (smg::readGxDrawState): generation mode, indirect stages, scissor, TEV orders, depth and
// blend modes, the texture maps' modes, sizes and addresses, TEV stages and colours, alpha test, depth texture, konst selects.
bool drawsReadRegister(std::uint32_t address) {
    if (address == 0x00 || address == 0x20 || address == 0x21 || address == 0x40 || address == 0x41) return true;
    if ((address >= 0x10 && address <= 0x1F) || (address >= 0x28 && address <= 0x2F) || (address >= 0xC0 && address <= 0xE7) ||
        (address >= 0xF3 && address <= 0xFD))
        return true;
    const std::uint32_t map = address & 0xDF;  // 0x80-0x9F and 0xA0-0xBF: the same layout for maps 0-3 and 4-7
    return (map >= 0x80 && map <= 0x83) || (map >= 0x88 && map <= 0x8B) || (map >= 0x94 && map <= 0x97);
}

// A component's bytes by its format: u8, s8, u16, s16, f32.
std::uint32_t componentSize(std::uint32_t format) { return format == 4 ? 4 : format >= 2 ? 2 : 1; }
// An attribute's components, its format chosen once per attribute rather than per component; fixed point is a multiply
// (exact for powers of two), and floats take no shift.
void components(const std::uint8_t* at, std::uint32_t format, std::uint32_t count, float scale, bool native, float* out) {
    switch (format) {
    case 0:
        for (std::uint32_t i = 0; i < count; ++i) out[i] = at[i] * scale;
        break;
    case 1:
        for (std::uint32_t i = 0; i < count; ++i) out[i] = static_cast<std::int8_t>(at[i]) * scale;
        break;
    case 2:
        for (std::uint32_t i = 0; i < count; ++i) out[i] = static_cast<float>(half(at + 2 * i, native)) * scale;
        break;
    case 3:
        for (std::uint32_t i = 0; i < count; ++i) out[i] = static_cast<std::int16_t>(half(at + 2 * i, native)) * scale;
        break;
    default:
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t bits = word(at + 4 * i, native);
            std::memcpy(&out[i], &bits, 4);
        }
    }
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

GxVertexLayout::GxVertexLayout(const GxCommandProcessor& gpu, std::uint8_t format) {
    const std::uint32_t lo = gpu.cp[VcdLo], hi = gpu.cp[VcdHi], a = gpu.cp[VatA + format], b = gpu.cp[VatB + format], c = gpu.cp[VatC + format];
    matrixIndex_ = field(lo, 0, 1);
    size = matrixIndex_;
    for (int i = 0; i < 8; ++i) {
        texMatrixIndexes_ |= static_cast<std::uint8_t>(field(lo, 1 + i, 1) << i);
        size += field(lo, 1 + i, 1);
    }
    const auto set = [&](Attribute& at, std::uint32_t type, int array, std::uint32_t fmt, std::uint32_t count, std::uint32_t shift, std::uint32_t direct) {
        at.type = static_cast<std::uint8_t>(type), at.format = static_cast<std::uint8_t>(fmt), at.count = static_cast<std::uint8_t>(count);
        at.shift = static_cast<std::uint8_t>(shift), at.componentSize = static_cast<std::uint8_t>(componentSize(fmt));
        at.base = gpu.cp[ArrayBase + array], at.stride = gpu.cp[ArrayStride + array], at.directSize = static_cast<std::uint8_t>(direct);
        at.scale = 1.0f / static_cast<float>(1u << at.shift);
        // The host's memory is one offset from the Wii's addresses, so an array's elements follow its base there too.
        if (type >= 2) at.array = gpu.memoryFunction()(at.base);
        size += type == 0 ? 0 : type == 1 ? direct : type == 2 ? 1 + at.skip : 2 + at.skip;
    };
    set(position_, field(lo, 9, 2), 0, field(a, 1, 3), field(a, 0, 1) + 2, field(a, 4, 5), (field(a, 0, 1) + 2) * componentSize(field(a, 1, 3)));
    {
        // Normals take a fixed point by their format: six fraction bits for s8, fourteen for s16.
        const std::uint32_t fmt = field(a, 10, 3), type = field(lo, 11, 2);
        const bool nbt = field(a, 9, 1), threeIndexes = nbt && field(a, 31, 1);
        if (threeIndexes && type >= 2) normal_.skip = static_cast<std::uint8_t>(2 * (type == 2 ? 1 : 2));  // binormal and tangent indexes, not kept
        set(normal_, type, 1, fmt, 3, fmt == 1 ? 6 : fmt == 3 ? 14 : 0, (nbt ? 9 : 3) * componentSize(fmt));
    }
    for (int channel = 0; channel < 2; ++channel) {
        const std::uint32_t fmt = field(a, 14 + 4 * channel, 3);
        set(color_[channel], field(lo, 13 + 2 * channel, 2), 2 + channel, fmt, 0, 0, colorSize(fmt));
    }
    // Each coordinate set's count, format and shift, from VAT A, B and C in turn.
    const std::uint32_t tex[8][3] = {{field(a, 21, 1), field(a, 22, 3), field(a, 25, 5)}, {field(b, 0, 1), field(b, 1, 3), field(b, 4, 5)},
                                     {field(b, 9, 1), field(b, 10, 3), field(b, 13, 5)}, {field(b, 18, 1), field(b, 19, 3), field(b, 22, 5)},
                                     {field(b, 27, 1), field(b, 28, 3), field(c, 0, 5)}, {field(c, 5, 1), field(c, 6, 3), field(c, 9, 5)},
                                     {field(c, 14, 1), field(c, 15, 3), field(c, 18, 5)}, {field(c, 23, 1), field(c, 24, 3), field(c, 27, 5)}};
    for (int i = 0; i < 8; ++i) {
        set(texcoord_[i], field(hi, 2 * i, 2), 4 + i, tex[i][1], tex[i][0] + 1, tex[i][2], (tex[i][0] + 1) * componentSize(tex[i][1]));
        if (texcoord_[i].type) texcoords_[texcoordCount_++] = static_cast<std::uint8_t>(i);
    }
}

// An attribute's own bytes: in the stream when direct, else in its array at the index the stream gives.
inline const std::uint8_t* GxVertexLayout::source(const Attribute& a, const std::uint8_t*& at) const {
    if (a.type == 1) {
        const std::uint8_t* here = at;
        at += a.directSize;
        return here;
    }
    const std::uint32_t index = a.type == 2 ? *at : half(at);
    at += (a.type == 2 ? 1 : 2) + a.skip;
    return a.array + index * a.stride;
}

void GxVertexLayout::read(const std::uint8_t* at, GxVertex& vertex) const {
    if (matrixIndex_) vertex.matrixIndex = *at++;
    if (texMatrixIndexes_)
        for (int i = 0; i < 8; ++i) if (texMatrixIndexes_ >> i & 1) vertex.texMatrixIndex[i] = *at++;
    if (position_.type) components(source(position_, at), position_.format, position_.count, position_.scale, position_.type != 1, vertex.position);
    if (normal_.type) components(source(normal_, at), normal_.format, 3, normal_.scale, normal_.type != 1, vertex.normal);
    for (int channel = 0; channel < 2; ++channel) {
        const Attribute& c = color_[channel];
        if (c.type) readColor(source(c, at), c.format, c.type != 1, vertex.color[channel]);
    }
    for (std::uint8_t k = 0; k < texcoordCount_; ++k) {
        const Attribute& t = texcoord_[texcoords_[k]];
        components(source(t, at), t.format, t.count, t.scale, t.type != 1, vertex.texcoord[texcoords_[k]]);
    }
}

bool GxCommandProcessor::drawsRead(std::uint32_t address) { return drawsReadRegister(address); }

const GxVertexLayout& GxCommandProcessor::layout(std::uint8_t format) const {
    format &= 7;
    if (layoutChanges_[format] != cpChanges) layouts_[format] = GxVertexLayout(*this, format), layoutChanges_[format] = cpChanges;
    return layouts_[format];
}

GxVertex GxCommandProcessor::readVertex(const std::uint8_t* at, std::uint8_t format) const {
    GxVertex vertex;
    layout(format).read(at, vertex);
    return vertex;
}

void GxCommandProcessor::loadXf(std::uint32_t address, std::uint32_t count, const std::uint8_t* from, bool native) {
    bool changed = false;
    for (std::uint32_t i = 0; i < count && address + i < sizeof xf / sizeof *xf; ++i) {
        const std::uint32_t value = word(from + i * 4, native);
        changed |= xf[address + i] != value;
        xf[address + i] = value;
    }
    xfChanges += changed;
}

void GxCommandProcessor::loadBp(std::uint32_t value) {
    const std::uint32_t address = value >> 24;
    if (address == 0xFE) {  // the mask for the next load only
        bpMask_ = value & 0xFFFFFF;
        return;
    }
    const std::uint32_t data = (bp[address] & ~bpMask_) | (value & bpMask_);
    bpMask_ = 0xFFFFFF;
    if (bp[address] != data && drawsReadRegister(address)) ++bpChanges;
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
        const std::uint32_t value = word(at + 2);
        if (cp[at[1]] != value) cp[at[1]] = value, ++cpChanges;
        return 6;
    }
    if (opcode == 0x10) {  // load XF registers: the count less one, then the first address
        if (available < 5) return 0;
        const std::uint32_t header = word(at + 1), count = (header >> 16) + 1, address = header & 0xFFFF;
        if (available < 5 + count * 4) return 0;
        loadXf(address, count, at + 5, false);
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
    if ((opcode & 0xE7) == 0x20) {  // load XF registers from arrays 12-15 (GXLoadPosMtxIndx and its kin): index, count less one, address
        if (available < 5) return 0;
        const std::uint32_t header = word(at + 1), index = header >> 16, count = (header >> 12 & 0xF) + 1, address = header & 0xFFF;
        const int array = 12 + (opcode >> 3 & 3);
        const std::uint8_t* from = host_.memory(cp[ArrayBase + array] + index * cp[ArrayStride + array]);
        if (!from) {
            host_.unsupported("an indexed XF load from outside memory", opcode);
            return 5;
        }
        // The CPU wrote these (J3D's matrices, GX's light objects): read in its order, as indexed vertex arrays are.
        loadXf(address, count, from, true);
        return 5;
    }
    host_.unsupported("an opcode the Wii's GPU does not know", opcode);
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
    // A command still short of the length its header gave waits without being read again (immediate-mode vertices
    // arrive a word at a time); a CP load in between may have changed a vertex's size, so that counts too.
    if (size < needed_[stream] && cpChanges == neededCp_[stream]) return;
    std::uint32_t read = 0;
    while (read < size) {
        const std::uint32_t used = command(pending + read, size - read);
        if (!used) break;
        read += used;
    }
    if (read) {
        std::memmove(pending, pending + read, size - read);
        size -= read;
    }
    needed_[stream] = size ? length(pending, size) : 0, neededCp_[stream] = cpChanges;
}

std::uint32_t GxCommandProcessor::length(const std::uint8_t* at, std::uint32_t available) const {
    const std::uint8_t opcode = at[0];
    if (opcode == 0x08) return 6;
    if (opcode == 0x10) return available < 5 ? 5 : 5 + ((word(at + 1) >> 16) + 1) * 4;
    if (opcode == 0x61 || (opcode & 0xE7) == 0x20) return 5;
    if (opcode == 0x40) return 9;
    if (opcode >= 0x80 && opcode < 0xC0) return available < 3 ? 3 : 3 + half(at + 1) * vertexSize(opcode & 7);
    return 1;
}
} // namespace smg
