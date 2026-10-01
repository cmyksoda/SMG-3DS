#include <smg/GxDraw.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::printf("FAIL: %s\n", message); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

struct Stream {
    std::vector<std::uint8_t> bytes;
    void u8(unsigned v) { bytes.push_back(static_cast<std::uint8_t>(v)); }
    void u16(unsigned v) { u8(v >> 8), u8(v); }
    void u32(std::uint32_t v) { u16(v >> 16), u16(v & 0xFFFF); }
    void f32(float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void cp(unsigned address, std::uint32_t value) { u8(0x08), u8(address), u32(value); }
    void bp(std::uint32_t value) { u8(0x61), u32(value); }
    void xf(unsigned address, std::initializer_list<std::uint32_t> values) {
        u8(0x10), u32(static_cast<std::uint32_t>(values.size() - 1) << 16 | address);
        for (std::uint32_t v : values) u32(v);
    }
    void xff(unsigned address, std::initializer_list<float> values) {
        u8(0x10), u32(static_cast<std::uint32_t>(values.size() - 1) << 16 | address);
        for (float f : values) f32(f);
    }
};

struct Drawn { std::uint8_t kind, format; std::uint16_t count; std::vector<std::uint8_t> data; };
std::vector<Drawn> drawn;
std::vector<std::uint8_t> memory(0x2000);
smg::GxCommandProcessor::Host host() {
    return {[](std::uint32_t physical) -> const std::uint8_t* { return memory.data() + physical; },
            [] {}, [](std::uint16_t, bool) {}, [](const char* what, std::uint32_t) { std::printf("unsupported: %s\n", what); std::exit(1); },
            [](std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices) {
                drawn.push_back({kind, format, count, {}});
                drawn.back().data.assign(vertices, vertices + count * 6);  // this file's formats are six bytes
            },
            [](std::uint32_t) {}};
}

// MainLoopFramework::clearEfb's state: C0 through one TEV stage, position u16 XY and one u8 ST coordinate direct,
// an orthographic projection over a 640x480 viewport, and depth replaced from a Z24X8 texture.
void clearState(Stream& s) {
    s.cp(0x50, 1 << 9), s.cp(0x60, 1), s.cp(0x70, 2 << 1 | 1 << 21);
    s.xff(0x000, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});                       // position matrix 0
    s.xff(60 * 4, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});                      // GX_IDENTITY
    s.xf(0x1018, {60 << 6}), s.xf(0x1009, {0}), s.xf(0x103F, {1}), s.xf(0x1040, {5 << 7}), s.xf(0x1012, {0});
    s.xff(0x1020, {2.0f / 640, -1, 2.0f / -480, 1, -1, -1});                  // C_MTXOrtho(0, 480, 0, 640, 0, 1)
    s.xf(0x1026, {1});
    s.xff(0x101A, {320, -240, 16777215, 342 + 320, 342 + 240, 16777215});   // GXSetViewport(0, 0, 640, 480, 0, 1)
    s.bp(0x00000001);                                                          // one texgen, no channels, one stage
    s.bp(0x280003C0);                                                          // stage 0: map 0, coord 0, no channel
    s.bp(0xC008FFF2), s.bp(0xC108FF90);                                        // (0, 0, 0, C0); (0, 0, 0, A0), clamped
    s.bp(0xE20FF040), s.bp(0xE30800C0);                                        // C0 = (0x40, 0x80, 0xC0, 0xFF)
    s.bp(0xF37F0000), s.bp(0x4000001F), s.bp(0x41000018);                      // alpha always; z always, updating; no blend
    s.bp(0xF4000000), s.bp(0xF500000A);                                        // depth replaced from a u24 texel
    s.bp(0x88600C03), s.bp(0x94000080), s.bp(0x80000030);                      // map 0: 4x4 format 6 at 0x1000
    s.bp(0x20156156), s.bp(0x213D5335);                                        // scissor over the whole 640x480
}
void quad(Stream& s, std::initializer_list<unsigned> corners) {
    s.u8(0x80), s.u16(4);
    for (unsigned c : corners) {
        const unsigned x = c & 1 ? 640 : 0, y = c & 2 ? 480 : 0;
        s.u16(x), s.u16(y), s.u8(c & 1), s.u8(c >> 1);
    }
}

// A stage offset by an indirect matrix keeps its own coordinates, its offset left out; wrapping or replacing them can't be.
void indirect() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0x00010001);  // GEN_MODE: one indirect stage
    s.bp(0x10000270);  // stage 0: indirect stage 0, biased S T U, matrix 0
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDraw d;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    check(!d.unsupported && d.indirectDropped && d.triangles.size() == 6, "an indirect offset is left out and the stage still drawn");
    gpu.bp[0x10] = 0x270 | 1 << 16;  // T wraps to 0: the lookup replaces the coordinate
    smg::GxDraw wrapped;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, wrapped);
    check(wrapped.unsupported != nullptr, "one that wraps its coordinates stops by name");
}

// GX lighting, by XF state as GXSetChanCtrl, GXSetChanMatColor, GXSetChanAmbColor and GXLoadLightObjImm leave it.
void setFloat(smg::GxCommandProcessor& gpu, unsigned address, float value) { std::memcpy(&gpu.xf[address], &value, 4); }
void lighting() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    gpu.xf[0x1009] = 1;
    gpu.xf[0x100E] = 1 << 1 | 1 << 2 | 2 << 7;  // colour: material register, lit, light 0, ambient register, diffuse clamped
    gpu.xf[0x1010] = 0;                          // alpha: the material register, unlit
    gpu.xf[0x100C] = 0xFFFFFF80, gpu.xf[0x100A] = 0x000000FF;
    gpu.xf[0x600 + 3] = 0xFFFFFFFF;
    setFloat(gpu, 0x600 + 12, 10);  // light 0 at (0, 0, 10)
    const float origin[3] = {0, 0, 0}, facing[3] = {0, 0, 1}, side[3] = {1, 0, 0}, tilted[3] = {0, 0.6f, 0.8f};
    const std::uint8_t vertex[4] = {10, 20, 30, 40};
    std::uint8_t c[4];
    smg::gxLitChannel(gpu, origin, facing, vertex, c);
    check(c[0] == 255 && c[1] == 255 && c[2] == 255 && c[3] == 0x80, "a white light straight on lights the material fully; alpha stays unlit");
    smg::gxLitChannel(gpu, origin, side, vertex, c);
    check(c[0] == 0, "a light at right angles adds nothing");
    smg::gxLitChannel(gpu, origin, tilted, vertex, c);
    check(c[0] == 204, "and one at an angle adds its cosine");
    gpu.xf[0x100A] = 0x404040FF;
    const float away[3] = {0, 0, -1};
    smg::gxLitChannel(gpu, origin, away, vertex, c);
    check(c[0] == 63, "facing away, clamped, only the ambient is left, through the GPU's (m * (l + l >> 7)) >> 8");
    gpu.xf[0x100A] = 0x000000FF;
    gpu.xf[0x100E] |= 1 << 9 | 1 << 10;  // spot attenuation
    setFloat(gpu, 0x600 + 4, 1), setFloat(gpu, 0x600 + 7, 1), setFloat(gpu, 0x600 + 9, 0.25f), setFloat(gpu, 0x600 + 15, -1);
    setFloat(gpu, 0x600 + 12, 2);
    smg::gxLitChannel(gpu, origin, facing, vertex, c);
    check(c[0] == 126, "a spot light two units off falls to half by K0 + K2 d^2");
    gpu.xf[0x1010] = 1 << 1 | 1 << 2 | 2 << 7;
    gpu.xf[0x100E] = 1, gpu.xf[0x100A] = 0;  // colour from the vertex, unlit; no ambient alpha
    gpu.xf[0x600 + 3] = 0xFFFFFF80;
    setFloat(gpu, 0x600 + 12, 10);
    smg::gxLitChannel(gpu, origin, facing, vertex, c);
    check(c[0] == 10 && c[2] == 30 && c[3] == (0x80 * (0x80 + 1) >> 8), "the colour can come from the vertex while the alpha is lit");
}

void clearQuad() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());  // 80 KiB: more than a 3DS thread's stack
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    check(drawn.size() == 1 && drawn[0].kind == 0x80 && drawn[0].count == 4, "the clear quad reaches the host");
    smg::GxDraw d;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    check(!d.unsupported, "clearEfb's state is all drawable");
    const smg::TevStage& t = d.material.stages[0];
    check(d.material.tevStageCount == 1 && t.colorIn[0] == 15 && t.colorIn[3] == 2 && t.alphaIn[0] == 7 && t.alphaIn[3] == 1 &&
          t.colorOp == 0 && t.colorClamp == 1 && t.colorReg == 0, "the TEV stage reads as GXSetTevColorIn and GXSetTevColorOp set it");
    check(t.texMap == 0 && t.texCoord == 0 && t.channel == 6 && d.material.textures[0] == 0, "the TEV order reads its map, coordinate and no channel");
    check(near(d.material.tevColors[0][0], 0x40 / 255.0f) && near(d.material.tevColors[0][1], 0x80 / 255.0f) &&
          near(d.material.tevColors[0][2], 0xC0 / 255.0f) && near(d.material.tevColors[0][3], 1), "C0 reads from its two halves");
    check(d.material.depth.enable && d.material.depth.function == 7 && d.material.depth.update && d.material.blend.type == 0 &&
          d.colorUpdate && d.alphaUpdate && d.material.alphaTest.compare0 == 7 && d.material.alphaTest.op == 1, "the pixel tests read as set");
    check(d.depthTextureOp == 2 && d.depthTextureType == 2, "depth comes from a 24-bit texel, replacing");
    check(d.textures[0].width == 4 && d.textures[0].height == 4 && d.textures[0].format == 6 && d.textures[0].address == 0x1000 &&
          d.textures[0].magFilter == 1, "texture map 0 reads its size, format, address and filter");
    check(d.scissor[0] == 0 && d.scissor[1] == 0 && d.scissor[2] == 640 && d.scissor[3] == 480, "the scissor covers the EFB");
    check(d.triangles.size() == 6, "a quad is two triangles");
    const float* p = d.triangles[0].position;
    check(near(p[0], -1) && near(p[1], 1) && near(p[2], -1) && near(p[3], 1), "the EFB's top left is the PICA's (-1, 1), at the near plane");
    p = d.triangles[2].position;
    check(near(p[0], 1) && near(p[1], -1), "the bottom right is (1, -1)");
    check(d.program.stageCount >= 1 && d.program.unitCount == 0, "the combiner program needs no texture unit for C0 alone");

    // Culling in EFB space: GEN_MODE's reject 1 drops what GX_CULL_BACK names, 2 what GX_CULL_FRONT does.
    for (unsigned reject : {1u, 2u}) {
        Stream c;
        c.bp(0x00000001 | reject << 14);
        quad(c, {0, 1, 3, 2});
        quad(c, {0, 2, 3, 1});
        drawn.clear();
        gpu.put(c.bytes.data(), static_cast<std::uint32_t>(c.bytes.size()));
        smg::GxDraw clockwise, other;
        smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, clockwise);
        smg::buildGxDraw(gpu, drawn[1].kind, drawn[1].format, drawn[1].count, drawn[1].data.data(), {}, other);
        check(reject == 2 ? clockwise.triangles.empty() && other.triangles.size() == 6 : other.triangles.empty() && clockwise.triangles.size() == 6,
              "GX_CULL_FRONT drops what is clockwise on screen, GX_CULL_BACK the other winding");
    }
}

// A TEV stage that samples texture 0 gets a combiner unit and the texgen's coordinates through GX_IDENTITY.
void texturedQuad() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());  // 80 KiB: more than a 3DS thread's stack
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);  // colour (0, 0, 0, TEXC)
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDraw d;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    check(d.program.unitCount == 1 && d.program.units[0].texture == 0 && d.program.units[0].texCoord == 0, "texture 0 takes a combiner unit");
    const smg::GxDrawVertex& corner = d.triangles[2];
    check(near(corner.texcoord[0][0], 1) && near(corner.texcoord[0][1], 1), "its coordinates come through the identity texgen");
}

// A perspective projection keeps w, and GX's depth range turns into citro3d's: near -1, far 0.
void perspective() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());  // 80 KiB: more than a 3DS thread's stack
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    const float n = 1, f = 10;
    s.xff(0x1020, {1, 0, 1, 0, -n / (f - n), -f * n / (f - n)}), s.xf(0x1026, {0});
    s.xff(0x000, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, -1});  // one unit in front of the camera
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDraw d;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    const float* p = d.triangles[0].position;
    check(near(p[3], 1) && near(p[2] / p[3], -1), "a point on the near plane has depth -1 and w 1");
    Stream far;
    far.xff(0x000, {10, 0, 0, 0, 0, 10, 0, 0, 0, 0, 1, -10});
    quad(far, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(far.bytes.data(), static_cast<std::uint32_t>(far.bytes.size()));
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    p = d.triangles[0].position;
    check(near(p[3], 10) && near(p[2] / p[3], 0), "a point on the far plane has depth 0");
}

// Colour channel 0 from its material register or from the vertex, as XF's control words choose.
void channels() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());  // 80 KiB: more than a 3DS thread's stack
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.xf(0x1009, {1}), s.xf(0x100C, {0x11223344}), s.xf(0x100E, {0}), s.xf(0x1010, {0});
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDraw d;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    const std::uint8_t* c = d.triangles[0].color;
    check(c[0] == 0x11 && c[1] == 0x22 && c[2] == 0x33 && c[3] == 0x44, "the material register colours the channel");
    Stream lit;
    lit.xf(0x100E, {2});  // lighting on
    quad(lit, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(lit.bytes.data(), static_cast<std::uint32_t>(lit.bytes.size()));
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    c = d.triangles[0].color;
    check(!d.unsupported && c[0] == 0 && c[2] == 0 && c[3] == 0x44, "lit with no lights and a black ambient, the colour is black; alpha stays the material's");
}
} // namespace

// nw4r's layout material: black (C0) to white (C1) by the texture, which is the texture itself. The material keeps
// C0, C1, C2, PREV, as the combiner reads them; BP 0xE0 is PREV's, so a grey there must not become C0.
void layoutMaterial() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008248F);                                    // colour: lerp(C0, C1, TEXC), clamped
    s.bp(0xE0080080), s.bp(0xE1080080);                  // PREV = grey
    s.bp(0xE20FF000), s.bp(0xE3000000);                  // C0 = black
    s.bp(0xE40FF0FF), s.bp(0xE50FF0FF);                  // C1 = white
    s.bp(0xF6000004), s.bp(0xF700000E);                  // swap table 0 as GXInit leaves it: R, G, B, A
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDraw d;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    const auto& c = d.material.tevColors;
    check(near(c[0][0], 0) && near(c[1][0], 1) && near(c[1][2], 1) && near(c[3][0], 0x80 / 255.0f), "the material holds C0, C1, then PREV last");
    bool plain = false, inverted = false;
    for (unsigned k = 0; k < d.program.stageCount; ++k)
        for (int i = 0; i < 3; ++i)
            if (d.program.stages[k].colorSource[i] == smg::CombineSource::Texture0) {
                plain |= d.program.stages[k].colorOperand[i] == smg::CombineOperand::Color;
                inverted |= d.program.stages[k].colorOperand[i] == smg::CombineOperand::OneMinusColor;
            }
    check(plain && !inverted, "black to white by the texture reads the texture as it is");
}

int runGxDrawTests() {
    clearQuad();
    indirect();
    lighting();
    layoutMaterial();
    texturedQuad();
    perspective();
    channels();
    std::printf("%u GX draw checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGxDrawTests(); }
#endif
