#include <smg/GxCommands.hpp>
#include <algorithm>
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
// blend modes, destination alpha, the texture maps' modes, sizes and addresses, TEV stages and colours, alpha test, depth texture, konst selects.
bool drawsReadRegister(std::uint32_t address) {
    if (address == 0x00 || address == 0x20 || address == 0x21 || address == 0x40 || address == 0x41 || address == 0x42) return true;
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
    words_[0] = lo, words_[1] = hi, words_[2] = a, words_[3] = b, words_[4] = c, read_ = true;
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
        at.arrayIndex = static_cast<std::uint8_t>(array);
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
        texcoord_[i].decode = pick(texcoord_[i]);
    }
    position_.decode = pick(position_), normal_.decode = pick(normal_);
}

template <int Kind, int Format, int Count>
void GxVertexLayout::decodeAs(const Attribute& a, const std::uint8_t*& at, float* out) {
    constexpr int size = Format == 4 ? 4 : Format >= 2 ? 2 : 1;
    constexpr bool native = Kind != 1;  // arrays are the CPU's (native here), the stream the GPU's
    const std::uint8_t* data;
    if constexpr (Kind == 1) {
        data = at, at += Count * size;
    } else if constexpr (Kind == 2) {
        data = a.array + *at * a.stride, at += 1;
    } else {
        data = a.array + (std::uint32_t(at[0]) << 8 | at[1]) * a.stride, at += 2;
    }
    for (int i = 0; i < Count; ++i) {
        const std::uint8_t* c = data + i * size;
        if constexpr (Format == 0) out[i] = c[0] * a.scale;
        else if constexpr (Format == 1) out[i] = static_cast<std::int8_t>(c[0]) * a.scale;
        else if constexpr (Format == 2) out[i] = static_cast<float>(half(c, native)) * a.scale;
        else if constexpr (Format == 3) out[i] = static_cast<std::int16_t>(half(c, native)) * a.scale;
        else {
            const std::uint32_t bits = word(c, native);
            std::memcpy(&out[i], &bits, 4);
        }
    }
}

void GxVertexLayout::decodeAny(const Attribute& a, const std::uint8_t*& at, float* out) {
    components(source(a, at), a.format, a.count, a.scale, a.type != 1, out);
}

GxVertexLayout::Decoder GxVertexLayout::pick(const Attribute& a) {
    // NBT's extra indexes, or its binormal and tangent in the stream, take the general path.
    if (!a.type || a.skip || a.format > 4 || a.count < 1 || a.count > 3 || (a.type == 1 && a.directSize != a.count * a.componentSize)) return &decodeAny;
#define SMG_DECODERS(kind, format) {&decodeAs<kind, format, 1>, &decodeAs<kind, format, 2>, &decodeAs<kind, format, 3>}
#define SMG_KIND(kind) {SMG_DECODERS(kind, 0), SMG_DECODERS(kind, 1), SMG_DECODERS(kind, 2), SMG_DECODERS(kind, 3), SMG_DECODERS(kind, 4)}
    static constexpr Decoder table[3][5][3] = {SMG_KIND(1), SMG_KIND(2), SMG_KIND(3)};
#undef SMG_KIND
#undef SMG_DECODERS
    return table[a.type - 1][a.format][a.count - 1];
}

// An attribute's own bytes: in the stream when direct, else in its array at the index the stream gives.
const std::uint8_t* GxVertexLayout::source(const Attribute& a, const std::uint8_t*& at) {
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
    if (position_.type) position_.decode(position_, at, vertex.position);
    if (normal_.type) normal_.decode(normal_, at, vertex.normal);
    for (int channel = 0; channel < 2; ++channel) {
        const Attribute& c = color_[channel];
        if (c.type) readColor(source(c, at), c.format, c.type != 1, vertex.color[channel]);
    }
    for (std::uint8_t k = 0; k < texcoordCount_; ++k) {
        const Attribute& t = texcoord_[texcoords_[k]];
        t.decode(t, at, vertex.texcoord[texcoords_[k]]);
    }
}

std::uint16_t GxVertexLayout::indexedArrays() const {
    std::uint16_t arrays = 0;
    const Attribute* attributes[12] = {&position_, &normal_, &color_[0], &color_[1], &texcoord_[0], &texcoord_[1], &texcoord_[2],
                                       &texcoord_[3], &texcoord_[4], &texcoord_[5], &texcoord_[6], &texcoord_[7]};
    for (int i = 0; i < 12; ++i)
        if (attributes[i]->type >= 2) arrays = static_cast<std::uint16_t>(arrays | 1u << i);
    return arrays;
}

void GxVertexLayout::indexedSpan(const std::uint8_t* at, const std::uint8_t*& lo, const std::uint8_t*& hi) const {
    at += matrixIndex_;
    for (int i = 0; i < 8; ++i) at += texMatrixIndexes_ >> i & 1;
    const auto widen = [&](const Attribute& a) {
        if (!a.type) return;
        if (a.type == 1) return void(at += a.directSize);
        const std::uint8_t* element = source(a, at);
        lo = std::min(lo, element), hi = std::max(hi, element + a.directSize);
    };
    widen(position_), widen(normal_), widen(color_[0]), widen(color_[1]);
    for (const Attribute& t : texcoord_) widen(t);
}

bool GxCommandProcessor::drawsRead(std::uint32_t address) { return drawsReadRegister(address); }

bool GxVertexLayout::refresh(const GxCommandProcessor& gpu, std::uint8_t format) {
    if (!read_ || words_[0] != gpu.cp[VcdLo] || words_[1] != gpu.cp[VcdHi] || words_[2] != gpu.cp[VatA + format] ||
        words_[3] != gpu.cp[VatB + format] || words_[4] != gpu.cp[VatC + format])
        return false;
    for (Attribute* at : {&position_, &normal_, &color_[0], &color_[1], &texcoord_[0], &texcoord_[1], &texcoord_[2], &texcoord_[3], &texcoord_[4],
                          &texcoord_[5], &texcoord_[6], &texcoord_[7]}) {
        if (at->type < 2) continue;
        at->base = gpu.cp[ArrayBase + at->arrayIndex], at->stride = gpu.cp[ArrayStride + at->arrayIndex];
        at->array = gpu.memoryFunction()(at->base);
    }
    return true;
}

const GxVertexLayout& GxCommandProcessor::layout(std::uint8_t format) const {
    format &= 7;
    if (layoutChanges_[format] != cpChanges) {
        if (!layouts_[format].refresh(*this, format)) layouts_[format] = GxVertexLayout(*this, format);
        layoutChanges_[format] = cpChanges;
    }
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
    if (opcode == 0x01) {                           // the port's own: whether texture copies stay on the GPU
        if (available < 2) return 0;
        if (!host_.copiesStay) host_.unsupported("the port's copy-mode command with no host for it", at[1]);
        else host_.copiesStay(at[1] != 0);
        return 2;
    }
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
        ++displayListCalls, displayListBytes += word(at + 5);
        if (!host_.displayList || !host_.displayList(word(at + 1), word(at + 5))) {
            ++displayListDepth;
            run(host_.memory(word(at + 1)), word(at + 5));
            --displayListDepth;
        }
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
    if (!size && count > 8) {
        // A buffer of whole commands (a flushed write buffer): read where it is, keeping only an unfinished tail.
        std::uint32_t read = 0;
        while (read < count) {
            const std::uint32_t used = command(bytes + read, count - read);
            if (!used) break;
            read += used;
        }
        bytes += read, count -= read;
        if (!count) return void(needed_[stream] = 0);
    }
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
    if (opcode == 0x01) return 2;
    if (opcode == 0x10) return available < 5 ? 5 : 5 + ((word(at + 1) >> 16) + 1) * 4;
    if (opcode == 0x61 || (opcode & 0xE7) == 0x20) return 5;
    if (opcode == 0x40) return 9;
    if (opcode >= 0x80 && opcode < 0xC0) return available < 3 ? 3 : 3 + half(at + 1) * vertexSize(opcode & 7);
    return 1;
}
} // namespace smg
