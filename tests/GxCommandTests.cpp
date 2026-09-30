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

smg::GxCommandProcessor::Host host() {
    return {[](std::uint32_t physical) -> const std::uint8_t* { return displayList.data() + physical; },
            [] { ++drawDones; },
            [](std::uint16_t token, bool interrupt) { tokens.emplace_back(token, interrupt); },
            [](const char* what, std::uint32_t) { unsupported = what; }};
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
    std::uint8_t draw[] = {0x90, 0x00, 0x03};  // triangles, format 0, three vertices
    gpu->put(draw, sizeof draw);
    check(unsupported.find("primitive") != std::string::npos, "a primitive is reported as not the port's yet");
    delete gpu;
}
} // namespace

int runGxCommandTests() {
    registerLoads();
    pixelEngine();
    displayLists();
    std::printf("%u GX command processor checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGxCommandTests(); }
#endif
