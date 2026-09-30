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
    // Format 1: position direct s16 XYZ with 8 fraction bits, colour 0 direct RGBA8888, texcoord 0 direct f32 ST.
    gpu->cp[0x50] = 1u << 9 | 1u << 13;             // VCD_LO: POS direct, COL0 direct
    gpu->cp[0x60] = 1u << 0;                        // VCD_HI: TEX0 direct
    gpu->cp[0x71] = 1u << 0 | 3u << 1 | 8u << 4     // VAT_A: XYZ, s16, shift 8
                  | 5u << 14                        //        colour 0 RGBA8888
                  | 1u << 21 | 4u << 22;            //        texcoord 0 ST, f32
    check(gpu->vertexSize(1) == 6 + 4 + 8, "a vertex's size follows its descriptor and format");
    std::vector<std::uint8_t> stream{0x91, 0x00, 0x02};  // triangles in format 1, two vertices (a test needs no whole triangle)
    const std::uint8_t first[] = {0x01, 0x80, 0xFF, 0x00, 0x00, 0x40,  // x 1.5, y -1.0, z 0.25
                                  0x10, 0x20, 0x30, 0x40,              // colour
                                  0x3F, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00};  // s 1.0, t 2.0
    stream.insert(stream.end(), first, first + sizeof first);
    stream.insert(stream.end(), first, first + sizeof first);
    gpu->put(stream.data(), static_cast<std::uint32_t>(stream.size()));
    check(primitives.size() == 1 && primitives[0].kind == 0x90 && primitives[0].format == 1 && primitives[0].count == 2,
          "a whole primitive is handed over with its kind, format and count");
    const smg::GxVertex vertex = gpu->readVertex(primitives[0].vertices, 1);
    check(vertex.position[0] == 1.5f && vertex.position[1] == -1.0f && vertex.position[2] == 0.25f, "s16 positions dequantized by their shift");
    check(vertex.color[0][0] == 0x10 && vertex.color[0][3] == 0x40, "an RGBA8888 colour read in order");
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
    delete gpu;
}
} // namespace

// BP 0xFE masks the next load only; colour and konst registers share 0xE0-0xE7 by their type bit; copies report.
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
          "a TEV register and a konst colour at the same address are kept apart");
    check(copies.size() == 1 && copies[0] == 0x004000, "an EFB copy reports its command");
    delete gpu;
}

int runGxCommandTests() {
    registerLoads();
    bpMasksAndCopies();
    pixelEngine();
    displayLists();
    vertices();
    std::printf("%u GX command processor checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGxCommandTests(); }
#endif
