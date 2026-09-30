#include <smg/Bmg.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

using Bytes = std::vector<std::uint8_t>;
void put16(Bytes& out, unsigned value) { out.insert(out.end(), {std::uint8_t(value >> 8), std::uint8_t(value)}); }
void put32(Bytes& out, std::uint32_t value) { put16(out, value >> 16); put16(out, value & 0xFFFF); }
std::uint16_t native16(const std::uint8_t* at) { std::uint16_t v; std::memcpy(&v, at, 2); return v; }
std::uint32_t native32(const std::uint8_t* at) { std::uint32_t v; std::memcpy(&v, at, 4); return v; }

// Messages at bytes 0 ("Hi", a colour tag, a wait tag) and 22 (a number tag, a sound tag), and a third
// message that starts at the 'i' of the first.
Bytes text() {
    Bytes t;
    put16(t, 'H'); put16(t, 'i');
    put16(t, 0x1A); t.insert(t.end(), {8, 255}); put16(t, 0); t.insert(t.end(), {3, 0});  // colour 3
    put16(t, 0x1A); t.insert(t.end(), {8, 1}); put16(t, 0); put16(t, 30);                // wait 30 frames
    put16(t, 0);
    put16(t, 0x1A); t.insert(t.end(), {14, 6}); put16(t, 1); put32(t, 0); put32(t, 0x10002);  // number, argument 0x10002
    put16(t, 0x1A); t.insert(t.end(), {10, 2}); put16(t, 0); put16(t, 'S'); put16(t, 'E');   // sound "SE"
    put16(t, 0);
    while (t.size() % 16) t.push_back(0);
    return t;
}

Bytes bmg(const Bytes& t, std::uint32_t message3 = 2) {
    Bytes b = {'M', 'E', 'S', 'G', 'b', 'm', 'g', '1'};
    put32(b, 0); put32(b, 4); b.push_back(2); b.resize(0x20);
    b.insert(b.end(), {'I', 'N', 'F', '1'}); put32(b, 16 + 3 * 12); put16(b, 3); put16(b, 12); put32(b, 0);
    const std::uint32_t offsets[] = {0, 22, message3};
    for (std::uint32_t offset : offsets) { put32(b, offset); put16(b, 0x0102); b.insert(b.end(), {6, 7, 8, 9, 10, 11}); }
    b.insert(b.end(), {'D', 'A', 'T', '1'}); put32(b, 8 + t.size()); b.insert(b.end(), t.begin(), t.end());
    b.insert(b.end(), {'F', 'L', 'W', '1'}); put32(b, 16 + 3 * 8 + 1 * 3 + 5); put16(b, 3); put16(b, 1); put32(b, 0);
    b.insert(b.end(), {1, 0}); put16(b, 1); put16(b, 2); put16(b, 0);            // message 1, next node 2
    b.insert(b.end(), {2, 0}); put16(b, 3); put16(b, 7); put16(b, 0);            // branch kind 3, argument 7
    b.insert(b.end(), {3, 1}); put16(b, 0); put32(b, 0x12345678);              // event 1, argument 0x12345678
    put16(b, 0x0102); b.push_back(5); b.resize(b.size() + 5);
    // FLI1 is never read, and on the disc runs past the file.
    b.insert(b.end(), {'F', 'L', 'I', '1'}); put32(b, 64); put16(b, 1); put16(b, 0x0800);
    std::memcpy(b.data() + 8, (Bytes{0, 0, std::uint8_t(b.size() >> 8), std::uint8_t(b.size())}).data(), 4);
    return b;
}

void converts() {
    Bytes b = bmg(text());
    check(!smg::nativeBmg(b.data(), b.size()), "a BMG with every kind the disc uses turns native");
    check(native32(b.data() + 0xC) == 4 && native32(b.data() + 0x20) == 0x494E4631, "the header and block magics read natively");
    const std::uint8_t* inf = b.data() + 0x20;
    check(native16(inf + 8) == 3 && native16(inf + 10) == 12, "INF1's count and item size read natively");
    check(native32(inf + 16 + 12) == 22 && native16(inf + 16 + 12 + 4) == 0x0102 && inf[16 + 12 + 6] == 6 && inf[16 + 12 + 11] == 11,
          "an INF1 item's offset and camera set read natively, its bytes as they were");
    const std::uint8_t* text = inf + native32(inf + 4) + 8;
    check(native16(text) == 'H' && native16(text + 2) == 'i', "characters read as native units");
    // As MessageEditorMessageTag reads a tag: from the unit after 0x1A.
    check(native16(text + 4) == 0x1A, "a tag's mark reads as a native unit");
    const std::uint8_t* colour = text + 6;
    check(colour[0] == 8 && colour[1] == 255 && native16(colour + 2) == 0 && colour[4] == 3,
          "a colour tag's length and group read as bytes, its type as a unit, its colour as a byte");
    const std::uint8_t* wait = text + 14;
    check(wait[0] == 8 && wait[1] == 1 && native16(wait + 4) == 30, "a wait tag's frames read as a native half");
    check(native16(text + 20) == 0, "the first message ends in a native 0");
    const std::uint8_t* number = text + 24;
    check(number[1] == 6 && native16(number + 2) == 1 && native32(number + 4) == 0 && native32(number + 8) == 0x10002,
          "a number tag's slot stays empty and its argument reads as a native word");
    const std::uint8_t* sound = text + 38;
    check(sound[1] == 2 && native16(sound + 4) == 'S' && native16(sound + 6) == 'E', "a sound tag's name reads as native units");
    const std::uint8_t* flw = inf + native32(inf + 4) + native32(inf + native32(inf + 4) + 4);
    check(native32(flw) == 0x464C5731 && native16(flw + 8) == 3 && native16(flw + 10) == 1, "FLW1's counts read natively");
    check(native16(flw + 16 + 2) == 1 && native16(flw + 16 + 4) == 2 && native16(flw + 16 + 6) == 0, "a message node's halves read natively");
    check(native16(flw + 24 + 2) == 3 && native16(flw + 24 + 4) == 7, "a branch node's halves read natively");
    check(flw[32 + 1] == 1 && native32(flw + 32 + 4) == 0x12345678, "an event node's argument reads as a native word");
    check(native16(flw + 40) == 0x0102 && flw[42] == 5, "the branch table reads natively, its byte table as it was");

    const Bytes once = b;
    check(!smg::nativeBmg(b.data(), b.size()) && b == once, "a native BMG is left as it is");
}

void refuses() {
    Bytes t = text();
    t[25] = 11;  // the number tag becomes a kind with parameters the port has no layout for
    Bytes b = bmg(t);
    const Bytes before = b;
    const char* why = smg::nativeBmg(b.data(), b.size());
    check(why && std::strstr(why, "group 11") && b == before, "an unknown tag kind is named and nothing changes");
    b = bmg(text(), 6);  // the third message starts on the colour tag's length and group
    check(smg::nativeBmg(b.data(), b.size()) && b == bmg(text(), 6), "text read as both bytes and units is refused");
}
} // namespace

int runBmgTests() {
    converts();
    refuses();
    std::printf("%u BMG text checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runBmgTests(); }
#endif
