#include <smg/GxCommands.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

// What the command processor reported, for the checks to read.
unsigned drawDones = 0;
std::vector<std::pair<unsigned, bool>> tokens;
std::string unsupported;
std::vector<std::uint8_t> displayList;
struct Primitive { unsigned kind, format, count; const std::uint8_t* vertices; };
std::vector<Primitive> primitives;
std::vector<std::uint32_t> copies;

smg::GxCommandProcessor::Host host() {
    return {[](std::uint32_t physical) -> const std::uint8_t* { return displayList.data() + physical; },
            [] { ++drawDones; },
            [](std::uint16_t token, bool interrupt) { tokens.emplace_back(token, interrupt); },
            [](const char* what, std::uint32_t) { unsupported = what; },
            [](std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices) {
                primitives.push_back({kind, format, count, vertices});
            },
            [](std::uint32_t command) { copies.push_back(command); }};
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void registerLoads() {
    auto* gpu = new smg::GxCommandProcessor(host());
    std::vector<std::uint8_t> stream;
    stream.push_back(0x61); put32(stream, 0x28123456);             // BP 0x28
    stream.push_back(0x08); stream.push_back(0x50); put32(stream, 0xCAFE0001);  // CP 0x50
    stream.push_back(0x10); put32(stream, (2 << 16) | 0x1008);     // XF 0x1008..0x100A
    put32(stream, 1); put32(stream, 2); put32(stream, 3);
    stream.push_back(0x00);                                        // NOP
    // Fed a byte at a time, as GX writes: a command waits until all of it has come.
    for (std::uint8_t byte : stream) gpu->put(&byte, 1);
    check(gpu->bp[0x28] == 0x123456, "a BP load keeps its 24 bits");
    check(gpu->cp[0x50] == 0xCAFE0001, "a CP load keeps its word");
    check(gpu->xf[0x1008] == 1 && gpu->xf[0x1009] == 2 && gpu->xf[0x100A] == 3, "an XF load of three fills three registers");
    check(gpu->pending() == 0 && unsupported.empty(), "every command taken, nothing left waiting");
    // A piece that ends inside a command: the next finishes it, and the rest of that piece is read where it is.
    stream.clear();
    stream.push_back(0x10); put32(stream, (2 << 16) | 0x1008);
    put32(stream, 4); put32(stream, 5); put32(stream, 6);
    stream.push_back(0x61); put32(stream, 0x2A654321);
    stream.push_back(0x61); put32(stream, 0x2B111111);
    gpu->put(stream.data(), 7);
    check(gpu->xf[0x1008] == 1 && gpu->pending() == 7, "half an XF load waits");
    gpu->put(stream.data() + 7, static_cast<std::uint32_t>(stream.size() - 7));
    check(gpu->xf[0x1008] == 4 && gpu->xf[0x100A] == 6 && gpu->bp[0x2A] == 0x654321 && gpu->bp[0x2B] == 0x111111 && gpu->pending() == 0,
          "the next piece finishes it and the commands after it");
    delete gpu;
}

// GXLoadPosMtxIndx: array 12 holds matrices the CPU computed, in its own order; the GPU takes a row of words from one.
void indexedLoads() {
    auto* gpu = new smg::GxCommandProcessor(host());
    displayList.assign(0x400, 0);
    for (std::uint32_t i = 0; i < 12; ++i) {
        const std::uint32_t value = 0x3F800000 + i;
        std::memcpy(&displayList[0x200 + 2 * 48 + i * 4], &value, 4);  // matrix 2 of a 48-byte stride, native words
    }
    const float light[3] = {1.5f, -2.0f, 0.25f};
    std::memcpy(&displayList[0x300 + 64], light, sizeof light);  // light 1, from the start of its element
    std::vector<std::uint8_t> stream;
    stream.push_back(0x08); stream.push_back(0xAC); put32(stream, 0x200);  // array 12's base and stride
    stream.push_back(0x08); stream.push_back(0xBC); put32(stream, 48);
    stream.push_back(0x20); put32(stream, 2u << 16 | 11u << 12 | 0x00C);  // matrix 2's twelve words to XF 0x00C
    stream.push_back(0x08); stream.push_back(0xAF); put32(stream, 0x300);  // array 15 (lights), as GXLoadLightObjIndx
    stream.push_back(0x08); stream.push_back(0xBF); put32(stream, 64);
    stream.push_back(0x38); put32(stream, 1u << 16 | 2u << 12 | 0x60A);   // its three position words to light 0's
    gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    bool rows = true;
    for (std::uint32_t i = 0; i < 12; ++i) rows = rows && gpu->xf[0x00C + i] == 0x3F800000 + i;
    check(rows && unsupported.empty(), "an indexed load copies the CPU's matrix words into XF in order");
    float position[3];
    std::memcpy(position, &gpu->xf[0x60A], sizeof position);
    check(position[0] == 1.5f && position[1] == -2.0f && position[2] == 0.25f, "and array D reaches the light objects");
    delete gpu;
}

void pixelEngine() {
    auto* gpu = new smg::GxCommandProcessor(host());
    std::vector<std::uint8_t> stream;
    stream.push_back(0x61); put32(stream, 0x48000000 | 0x1234);  // PE_TOKEN_INT
    stream.push_back(0x61); put32(stream, 0x47000000 | 0x1234);  // PE_TOKEN
    stream.push_back(0x61); put32(stream, 0x45000002);           // PE_DONE
    gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    check(tokens.size() == 2 && tokens[0] == std::make_pair(0x1234u, true) && tokens[1] == std::make_pair(0x1234u, false),
          "the token commands report the token, one with its interrupt");
    check(drawDones == 1, "draw-done is reported once");
    delete gpu;
}

void displayLists() {
    auto* gpu = new smg::GxCommandProcessor(host());
    displayList.assign(8, 0);  // a list at "physical" 8, as the host maps it
    displayList.push_back(0x61); put32(displayList, 0x30ABCDEF);
    displayList.push_back(0x00); displayList.push_back(0x00);
    std::vector<std::uint8_t> call{0x40};
    put32(call, 8); put32(call, 7);
    gpu->put(call.data(), static_cast<std::uint32_t>(call.size()));
    check(gpu->bp[0x30] == 0xABCDEF, "a called display list's commands run");
    delete gpu;
}

// Vertex formats as GX sets them, by the field positions of the SDK's own private/cp_reg.h.
void vertices() {
    auto* gpu = new smg::GxCommandProcessor(host());
    // Format 1: position direct s16 XYZ with 8 fraction bits, color 0 direct RGBA8888, texcoord 0 direct f32 ST.
    gpu->cp[0x50] = 1u << 9 | 1u << 13;             // VCD_LO: POS direct, COL0 direct
    gpu->cp[0x60] = 1u << 0;                        // VCD_HI: TEX0 direct
    gpu->cp[0x71] = 1u << 0 | 3u << 1 | 8u << 4     // VAT_A: XYZ, s16, shift 8
                  | 5u << 14                        //        color 0 RGBA8888
                  | 1u << 21 | 4u << 22;            //        texcoord 0 ST, f32
    check(gpu->vertexSize(1) == 6 + 4 + 8, "a vertex's size follows its descriptor and format");
    std::vector<std::uint8_t> stream{0x91, 0x00, 0x02};  // triangles in format 1, two vertices (a test needs no whole triangle)
    const std::uint8_t first[] = {0x01, 0x80, 0xFF, 0x00, 0x00, 0x40,  // x 1.5, y -1.0, z 0.25
                                  0x10, 0x20, 0x30, 0x40,              // color
                                  0x3F, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00};  // s 1.0, t 2.0
    stream.insert(stream.end(), first, first + sizeof first);
    stream.insert(stream.end(), first, first + sizeof first);
    gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    check(primitives.size() == 1 && primitives[0].kind == 0x90 && primitives[0].format == 1 && primitives[0].count == 2,
          "a whole primitive is handed over with its kind, format and count");
    const smg::GxVertex vertex = gpu->readVertex(primitives[0].vertices, 1);
    check(vertex.position[0] == 1.5f && vertex.position[1] == -1.0f && vertex.position[2] == 0.25f, "s16 positions dequantized by their shift");
    check(vertex.color[0][0] == 0x10 && vertex.color[0][3] == 0x40, "an RGBA8888 color read in order");
    check(vertex.texcoord[0][0] == 1.0f && vertex.texcoord[0][1] == 2.0f, "f32 texture coordinates read as floats");
    // Format 2: position indexed by u8 into array 0, stride 12, f32 XYZ.
    gpu->cp[0x50] = 2u << 9;
    gpu->cp[0x60] = 0;
    gpu->cp[0x72] = 1u << 0 | 4u << 1;
    displayList.assign(64, 0);
    // Arrays are memory the CPU wrote, so in its own order (the stream's index stays the GPU's).
    const float second[] = {10.0f, -3.0f, 0.5f};  // element 1
    std::memcpy(displayList.data() + 16 + 12, second, sizeof second);
    gpu->cp[0xA0] = 16;
    gpu->cp[0xB0] = 12;
    check(gpu->vertexSize(2) == 1, "an index8 attribute takes one byte");
    const std::uint8_t index = 1;
    const smg::GxVertex indexed = gpu->readVertex(&index, 2);
    check(indexed.position[0] == 10.0f && indexed.position[1] == -3.0f && indexed.position[2] == 0.5f,
          "an indexed position read from its array at base plus index times stride, in the CPU's order");
    // J3D moves the arrays before each shape: the layout follows the new base without being read again.
    const float moved[] = {-7.0f, 4.0f, 2.0f};
    std::memcpy(displayList.data() + 40 + 12, moved, sizeof moved);
    gpu->cp[0xA0] = 40, ++gpu->cpChanges;
    const smg::GxVertex follows = gpu->readVertex(&index, 2);
    check(follows.position[0] == -7.0f && follows.position[2] == 2.0f, "a layout follows its array's new base");
    gpu->cp[0x50] = 3u << 9, ++gpu->cpChanges;
    check(gpu->vertexSize(2) == 2, "and is read again when the descriptor changes (index16)");
    delete gpu;
}
} // namespace

// BP 0xFE masks the next load only; color and konst registers share 0xE0-0xE7 by their type bit; copies report.
void bpMasksAndCopies() {
    auto* gpu = new smg::GxCommandProcessor(host());
    std::vector<std::uint8_t> stream;
    for (std::uint32_t value : {0xC0123456u, 0xFE00FF00u, 0xC0ABCDEFu, 0xC0000001u}) { stream.push_back(0x61); put32(stream, value); }
    gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    check(gpu->bp[0xC0] == 0x000001, "a masked load changes only the mask's bits, and the mask lasts one load");
    stream.clear();
    for (std::uint32_t value : {0xFE00FF00u, 0xC012CD56u}) { stream.push_back(0x61); put32(stream, value); }
    gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    check(gpu->bp[0xC0] == 0x00CD01, "a mask keeps the register's other bits");
    stream.clear();
    for (std::uint32_t value : {0xE20FF0FFu, 0xE2800080u, 0xE3000040u, 0x52004000u}) { stream.push_back(0x61); put32(stream, value); }
    gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    check(gpu->tevColor[2] == 0x0FF0FF && gpu->tevKonst[2] == 0x800080 && gpu->tevColor[3] == 0x000040,
          "a TEV register and a konst color at the same address are kept apart");
    check(copies.size() == 1 && copies[0] == 0x004000, "an EFB copy reports its command");
    const auto load = [&](std::uint32_t value) {
        stream.clear(), stream.push_back(0x61), put32(stream, value);
        gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    };
    const std::uint32_t all = gpu->bpChanges, beside = gpu->bpChangesBesideColors, color1 = gpu->color1Changes;
    load(0xE2000011u), load(0xE3001100u);
    check(gpu->bpChanges == all + 2 && gpu->bpChangesBesideColors == beside, "a new C0 counts as a change, but not beside the colors");
    load(0xE2800081u);
    check(gpu->bpChangesBesideColors == beside + 1, "a konst color at C0's address is beside them");
    load(0xE2800081u), load(0xE2000011u);
    check(gpu->bpChangesBesideColors == beside + 1, "and writing either one again as it was changes nothing beside them");
    load(0xE4000022u);
    check(gpu->bpChangesBesideColors == beside + 1 && gpu->color1Changes == color1 + 1, "C1 counts apart");
    load(0xE6000022u);
    check(gpu->bpChangesBesideColors == beside + 2, "and C2 beside them");
    delete gpu;
}

// Two writers: a command half-written in one stream waits there while another stream's whole command applies.
void streams() {
    auto* gpu = new smg::GxCommandProcessor(host());
    const std::uint8_t xf[] = {0x10, 0x00, 0x00, 0x10, 0x00, 0x12, 0x34, 0x56, 0x78};  // one XF word at 0x1000
    const std::uint8_t bp[] = {0x61, 0x28, 0x00, 0x00, 0x01};
    gpu->put(0, xf, 3);
    gpu->put(1, bp, sizeof bp);
    check(gpu->bp[0x28] == 1 && gpu->xf[0x1000] == 0, "a whole command from another stream applies while one waits half-written");
    gpu->put(0, xf + 3, sizeof xf - 3);
    check(gpu->xf[0x1000] == 0x12345678 && gpu->pending(0) == 0 && gpu->pending(1) == 0, "and the waiting one applies once its stream completes it");
    delete gpu;
}

namespace {
std::vector<bool> copyModes, stayingAtCopy;

// The port's command 0x01 switches whether copies stay on the GPU, in order with the copies around it.
void copyMode() {
    smg::GxCommandProcessor::Host withMode = host();
    withMode.copiesStay = [](bool stay) { copyModes.push_back(stay); };
    withMode.copy = [](std::uint32_t) { stayingAtCopy.push_back(!copyModes.empty() && copyModes.back()); };
    auto* gpu = new smg::GxCommandProcessor(withMode);
    const std::uint8_t stream[] = {0x01, 0x01, 0x61, 0x52, 0x00, 0x00, 0x01, 0x01, 0x00, 0x61, 0x52, 0x00, 0x00, 0x01};
    for (std::uint8_t byte : stream) gpu->put(&byte, 1);
    check(copyModes.size() == 2 && copyModes[0] && !copyModes[1], "the copy mode turns on, then off, as the stream says");
    check(stayingAtCopy.size() == 2 && stayingAtCopy[0] && !stayingAtCopy[1], "each copy sees the mode set before it");
    check(gpu->pending() == 0 && unsupported.empty(), "the port's command is taken whole");
    delete gpu;
}

// Every kind (in the stream big-endian, by an 8-bit index, by a 16-bit one), format and component count a position or
// texture coordinate can take reads the same values: indexed arrays are the CPU's (native), the stream the GPU's.
void vertexDecoders() {
    auto* gpu = new smg::GxCommandProcessor(host());
    const unsigned sizes[5] = {1, 1, 2, 2, 4};
    unsigned cases = 0;
    for (unsigned format = 0; format < 5; ++format)
        for (unsigned count = 1; count <= 3; ++count)
            for (unsigned kind = 1; kind <= 3; ++kind) {
                const bool position = count >= 2, shifted = format < 4;  // positions take XY or XYZ, texcoord 0 S or ST
                const unsigned shift = shifted ? 3 : 0, size = sizes[format], stride = count * size + 4;
                float want[3];
                std::uint8_t bigEndian[12], native[12];
                for (unsigned i = 0; i < count; ++i) {
                    const int raw[5] = {200 - int(i) * 10, -50 + int(i) * 7, 50000 - int(i), -12345 + int(i) * 1000, 0};
                    std::uint32_t bits = static_cast<std::uint32_t>(raw[format]);
                    want[i] = static_cast<float>(raw[format]) / static_cast<float>(1u << shift);
                    if (format == 4) want[i] = 1.5f + static_cast<float>(i), std::memcpy(&bits, &want[i], 4);
                    for (unsigned b = 0; b < size; ++b) {
                        bigEndian[i * size + b] = static_cast<std::uint8_t>(bits >> (8 * (size - 1 - b)));
                        native[i * size + b] = static_cast<std::uint8_t>(bits >> (8 * b));
                    }
                }
                displayList.assign(0x200, 0);
                std::memcpy(displayList.data() + 0x100 + 2 * stride, native, count * size);
                std::vector<std::uint8_t> stream;
                const auto cpLoad = [&](unsigned address, std::uint32_t value) { stream.push_back(0x08), stream.push_back(address), put32(stream, value); };
                if (position) {
                    cpLoad(0x50, kind << 9), cpLoad(0x60, 0);
                    cpLoad(0x70, (count == 3) | format << 1 | shift << 4);
                    cpLoad(0xA0, 0x100), cpLoad(0xB0, stride);
                } else {
                    cpLoad(0x50, 0), cpLoad(0x60, kind);
                    cpLoad(0x70, (count == 2) << 21 | format << 22 | shift << 25);
                    cpLoad(0xA4, 0x100), cpLoad(0xB4, stride);
                }
                gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
                std::vector<std::uint8_t> vertex;
                if (kind == 1) vertex.assign(bigEndian, bigEndian + count * size);
                else if (kind == 2) vertex.push_back(2);
                else vertex.push_back(0), vertex.push_back(2);
                check(gpu->vertexSize(0) == vertex.size(), "the vertex size matches its kind and format");
                const smg::GxVertex v = gpu->readVertex(vertex.data(), 0);
                const float* got = position ? v.position : v.texcoord[0];
                bool same = true;
                for (unsigned i = 0; i < count; ++i) same &= got[i] == want[i];
                check(same, "every kind, format and count decodes to the same values");
                ++cases;
            }
    check(cases == 45, "all 45 decoders were read");
    delete gpu;
}
} // namespace

int runGxCommandTests() {
    streams();
    registerLoads();
    indexedLoads();
    bpMasksAndCopies();
    pixelEngine();
    displayLists();
    vertices();
    vertexDecoders();
    copyMode();
    std::printf("%u GX command processor checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGxCommandTests(); }
#endif
