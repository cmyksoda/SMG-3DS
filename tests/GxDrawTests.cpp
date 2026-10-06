#include <smg/GxDraw.hpp>
#include <smg/GxPrivate.h>
#include <algorithm>
#include <array>
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
std::vector<std::uint8_t> memory(0x20000);
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
    gpu.bp[0x10] = 0x270 | 6 << 13 | 6 << 16;  // GX_ITW_0 both ways: the lookup is the whole coordinate, a dependent read
    smg::GxDraw replaced;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, replaced);
    check(!replaced.unsupported && replaced.leftOut, "one the lookup replaces is left out, by name");
    gpu.bp[0x10] = 0x270 | 6 << 13 | 6 << 16 | 1 << 20;  // the same, added to the previous stage's
    smg::GxDraw chained;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, chained);
    check(chained.unsupported && !chained.leftOut, "but chained to another stage it still stops");
}

// GX lighting, by XF state as GXSetChanCtrl, GXSetChanMatColor, GXSetChanAmbColor and GXLoadLightObjImm leave it.
void setFloat(smg::GxCommandProcessor& gpu, unsigned address, float value) { std::memcpy(&gpu.xf[address], &value, 4); }
void lighting() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    gpu.xf[0x1009] = 1;
    gpu.xf[0x100E] = 1 << 1 | 1 << 2 | 2 << 7;  // color: material register, lit, light 0, ambient register, diffuse clamped
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
    gpu.xf[0x100E] = 1, gpu.xf[0x100A] = 0;  // color from the vertex, unlit; no ambient alpha
    gpu.xf[0x600 + 3] = 0xFFFFFF80;
    setFloat(gpu, 0x600 + 12, 10);
    smg::gxLitChannel(gpu, origin, facing, vertex, c);
    check(c[0] == 10 && c[2] == 30 && c[3] == (0x80 * (0x80 + 1) >> 8), "the color can come from the vertex while the alpha is lit");
}

// GXSetTevOp's tables: register words built from bitfields, which have to come out as the hardware's.
void tevOpTables() {
    const tev_color_env_t modulate = {0xC0, 0, 0, 1, 0, 0, 15, 8, 10, 15};  // GX_MODULATE, stage 0: ZERO, TEXC, RASC, ZERO
    const tev_alpha_env_t alpha = {0xC1, 0, 0, 1, 0, 0, 7, 4, 5, 7, 0, 0};    // ZERO, TEXA, RASA, ZERO
    check(smgTevColorWord(&modulate) == 0xC008F8AF && smgTevAlphaWord(&alpha) == 0xC108F2F0,
          "GX_MODULATE's table entries turn into the hardware's register words");
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
    s.bp(0xC008FFF8);  // color (0, 0, 0, TEXC)
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

// Color channel 0 from its material register or from the vertex, as XF's control words choose.
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
    check(c[0] == 0x11 && c[1] == 0x22 && c[2] == 0x33 && c[3] == 0x44, "the material register colors the channel");
    Stream lit;
    lit.xf(0x100E, {2});  // lighting on
    quad(lit, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(lit.bytes.data(), static_cast<std::uint32_t>(lit.bytes.size()));
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, d);
    c = d.triangles[0].color;
    check(!d.unsupported && c[0] == 0 && c[2] == 0 && c[3] == 0x44, "lit with no lights and a black ambient, the color is black; alpha stays the material's");
}

// One pipeline across primitives: it keeps each matrix slot's transform until XF changes, turns positions for the 3DS's
// screen when asked, and numbers its indices from where the caller's buffer is. Only BP registers a draw reads count as changes.
void pipelineAcrossPrimitives() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxDrawVertex v[4];
    std::uint16_t indices[12];
    check(pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format), "the clear quad prepares");
    std::size_t n = pipeline.run(drawn[0].data.data(), 4, v, 10, indices);
    check(n == 6 && indices[0] == 10 && indices[5] == 13, "indices count from the first vertex's number");
    check(near(v[0].position[0], -1) && near(v[0].position[1], 1), "the top left corner is (-1, 1)");

    Stream moved;
    moved.xff(0x000, {1, 0, 0, 320, 0, 1, 0, 0, 0, 0, 1, 0});  // half the EFB to the right
    gpu.put(moved.bytes.data(), static_cast<std::uint32_t>(moved.bytes.size()));
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(near(v[0].position[0], 0) && near(v[0].position[1], 1), "a matrix loaded between primitives moves the next one");

    smg::GxFrame turned;
    turned.quarterTurn = true;
    pipeline.prepare(gpu, state, turned, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(near(v[0].position[0], 1) && near(v[0].position[1], 0), "turned a quarter, x takes y and y takes minus x");

    const std::uint32_t before = gpu.bpChanges;
    Stream events;
    events.bp(0x47000001), events.bp(0x45000002), events.bp(0x0F000000);  // a token, draw done, an indirect mask
    events.bp(0xC008FFF2);                                                // stage 0's color, as it already is
    gpu.put(events.bytes.data(), static_cast<std::uint32_t>(events.bytes.size()));
    check(gpu.bpChanges == before, "events and loads that change nothing a draw reads leave the count");
    Stream changed;
    changed.bp(0xC008FFF8);
    gpu.put(changed.bytes.data(), static_cast<std::uint32_t>(changed.bytes.size()));
    check(gpu.bpChanges == before + 1, "a TEV stage that changes counts");
}

// A texgen through a texture matrix and the dual transform, as one folded map; loading another matrix between
// primitives changes the next one's coordinates.
void dualTexgen() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);                                          // color (0, 0, 0, TEXC): texture 0 takes a unit
    s.xff(30 * 4, {2, 0, 0, 0.5f, 0, 3, 0, 0.25f});            // texture matrix at row 30, two rows (ST)
    s.xf(0x1018, {30 << 6});                                   // texgen 0's matrix
    s.xf(0x1012, {1}), s.xf(0x1050, {0});                      // dual transform on, post matrix 0, not normalized
    s.xff(0x500, {1, 0, 0, 0.1f, 0, 1, 0, 0.2f, 0, 0, 1, 0});  // post matrix 0: an offset
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxDrawVertex v[4];
    std::uint16_t indices[12];
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(near(v[2].texcoord[0][0], 2.6f) && near(v[2].texcoord[0][1], 3.45f), "the texture matrix, then the post matrix's offset");
    Stream other;
    other.xff(30 * 4, {1, 0, 0, 0, 0, 1, 0, 0});
    gpu.put(other.bytes.data(), static_cast<std::uint32_t>(other.bytes.size()));
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(near(v[2].texcoord[0][0], 1.1f) && near(v[2].texcoord[0][1], 1.2f), "a texture matrix loaded between primitives takes effect");
    Stream position;
    position.xff(0, {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0});
    gpu.put(position.bytes.data(), static_cast<std::uint32_t>(position.bytes.size()));
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(near(v[2].texcoord[0][0], 1.1f) && near(v[2].texcoord[0][1], 1.2f), "a position matrix loaded between them leaves the fold as it was");
    Stream post;
    post.xff(0x500, {1, 0, 0, 0.5f, 0, 1, 0, 0.5f, 0, 0, 1, 0});
    gpu.put(post.bytes.data(), static_cast<std::uint32_t>(post.bytes.size()));
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(near(v[2].texcoord[0][0], 1.5f) && near(v[2].texcoord[0][1], 1.5f), "and a post matrix loaded between them takes effect");
}

// A unit whose texture sits in an atlas: its coordinates move into the texture's place, and coordinates past the
// texture, where the atlas would sample a neighbor, are told.
void atlasPlace() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);                         // texture 0 takes a unit
    s.xff(30 * 4, {1, 0, 0, 0, 0, 1, 0, 0});  // texture matrix at row 30: identity
    s.xf(0x1018, {30 << 6});
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxDrawVertex plain[4], placed[4];
    std::uint16_t indices[12];
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, plain, 0, indices);
    const float place[4] = {0.25f, 0.5f, 0.5f, 0.125f};
    pipeline.setAtlas(0, place);
    pipeline.run(drawn[0].data.data(), 4, placed, 0, indices);
    bool mapped = true;
    for (int i = 0; i < 4; ++i)
        mapped &= near(placed[i].texcoord[0][0], plain[i].texcoord[0][0] * 0.25f + 0.5f) && near(placed[i].texcoord[0][1], plain[i].texcoord[0][1] * 0.5f + 0.125f);
    check(mapped && !pipeline.takeAtlasOutside(), "an atlas member's coordinates go into its place");
    Stream wide;
    wide.xff(30 * 4, {2, 0, 0, 0, 0, 1, 0, 0});
    gpu.put(wide.bytes.data(), static_cast<std::uint32_t>(wide.bytes.size()));
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, placed, 0, indices);
    check(pipeline.takeAtlasOutside() && !pipeline.takeAtlasOutside(), "coordinates past the texture are told, once");
}

// A program that reads C0 but not the rasterized color takes C0 from the vertex color, so primitives whose C0 alone
// differs share one program; one that reads the rasterized color keeps C0 a constant.
void color0Rides() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);                                 // color (0, 0, 0, C0), alpha (0, 0, 0, A0), no channels
    s.bp(0xF6000004), s.bp(0xF700000E);            // swap table 0: R, G, B, A
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    check(state.color0InVertex && state.program.stages[0].colorSource[0] == smg::CombineSource::Primary, "C0 is read from the vertex color");
    smg::GxVertexPipeline pipeline;
    smg::GxDrawVertex v[4];
    std::uint16_t indices[12];
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(v[0].color[0] == 0x40 && v[0].color[1] == 0x80 && v[0].color[2] == 0xC0 && v[0].color[3] == 0xFF, "every vertex carries C0");
    Stream fade;
    fade.bp(0xE2020010);  // C0's red 0x10, alpha 0x20
    gpu.put(fade.bytes.data(), static_cast<std::uint32_t>(fade.bytes.size()));
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(v[3].color[0] == 0x10 && v[3].color[3] == 0x20, "a new C0 reaches the next primitive under the same state");
    Stream raster;
    raster.bp(0xC008FFFA);  // color (0, 0, 0, RASC)
    gpu.put(raster.bytes.data(), static_cast<std::uint32_t>(raster.bytes.size()));
    smg::readGxDrawState(gpu, state);
    check(!state.color0InVertex, "a program that reads the rasterized color keeps C0 a constant");
}

// With C0 riding, a program that also reads C1 takes it from a palette unit when the CPU path asks, so primitives whose
// C1 alone differs share one program too; the GPU path's state keeps C1 a constant.
void color1Rides() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xF6000004), s.bp(0xF700000E);  // swap table 0: R, G, B, A
    s.bp(0xC008428F);                    // color lerp(C1, C0, TEXC)
    s.bp(0xE4000033), s.bp(0xE5022011);  // C1 (0x33, 0x22, 0x11, 0)
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state, true);
    check(state.color0InVertex && state.color1InTexture && state.program.unitCount == 2 &&
              state.program.units[state.paletteUnit].texture == smg::CombinerProgram::Palette && state.program.exact,
          "C1 is read from a palette unit, beside the stage's own texture");
    smg::GxVertexPipeline pipeline;
    smg::GxDrawVertex v[4];
    std::uint16_t indices[12];
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    pipeline.setPaletteTexel(0.3125f, 0.6875f);
    pipeline.run(drawn[0].data.data(), 4, v, 0, indices);
    check(v[2].texcoord[state.paletteUnit][0] == 0.3125f && v[2].texcoord[state.paletteUnit][1] == 0.6875f,
          "every vertex carries the palette texel's coordinates");
    check(pipeline.meshEligible() != smg::GxVertexPipeline::MeshOk, "and such a state is the CPU path's alone");
    smg::GxDrawState constant;
    smg::readGxDrawState(gpu, constant);
    check(constant.color0InVertex && !constant.color1InTexture && constant.program.unitCount == 1, "the GPU path's state keeps C1 a constant");
    Stream writes;
    writes.bp(0xC088428F);  // the same stage, its result into C1
    gpu.put(writes.bytes.data(), static_cast<std::uint32_t>(writes.bytes.size()));
    smg::readGxDrawState(gpu, state, true);
    check(!state.color1InTexture, "a program that writes C1 keeps it where the combiner leaves it");
}

// Vertices decoded once (a small display list's) go through exactly the work its bytes would.
void decodedVertices() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);  // color (0, 0, 0, TEXC): texture 0 through the identity texgen
    quad(s, {0, 1, 3, 2});
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    pipeline.prepare(gpu, state, {}, drawn[0].kind, drawn[0].format);
    smg::GxDrawVertex bytes[4] = {}, decoded[4] = {};
    std::uint16_t fromBytes[12] = {}, fromDecoded[12] = {};
    const std::size_t n = pipeline.run(drawn[0].data.data(), 4, bytes, 0, fromBytes);
    smg::GxVertex read[4];
    for (int i = 0; i < 4; ++i) read[i] = gpu.readVertex(drawn[0].data.data() + i * gpu.vertexSize(drawn[0].format), drawn[0].format);
    check(pipeline.runDecoded(read, 4, decoded, 0, fromDecoded) == n && n == 6 && !std::memcmp(bytes, decoded, sizeof bytes) &&
              !std::memcmp(fromBytes, fromDecoded, sizeof fromBytes),
          "decoded vertices come out as the bytes do");
}

// The span pass finds the array memory a list's indexed vertices read, as the full build does, without decoding them.
void meshSpans() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);                                       // color (0, 0, 0, TEXC)
    s.cp(0x50, 2 << 9), s.cp(0x60, 0);                      // positions by an 8-bit index alone
    s.cp(0xA0, 0x1800), s.cp(0xB0, 12);                     // into array 0
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    Stream list;
    list.u8(0x90), list.u16(3), list.u8(2), list.u8(0), list.u8(1);
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxMesh mesh;
    const std::uint8_t *lo = nullptr, *hi = nullptr;
    check(pipeline.prepare(gpu, state, {}, 0x90, 0) && pipeline.meshSpan(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), lo, hi) &&
              pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) == smg::GxVertexPipeline::MeshOk &&
              lo == mesh.arraysLo && hi == mesh.arraysHi && lo == memory.data() + 0x1800 && hi > lo + 2 * 12,
          "the span pass finds the array memory the build reads");
    Stream state0;
    state0.bp(0x00000001);
    check(!pipeline.meshSpan(state0.bytes.data(), static_cast<std::uint32_t>(state0.bytes.size()), lo, hi), "and refuses a list of anything but primitives");
}

// The GPU path: a display list of primitives decodes once into model-space vertices and triangles; its uniforms move a
// vertex where the CPU path would, and a list with anything but primitives stays the CPU's.
void meshFromDisplayList() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);  // color (0, 0, 0, TEXC): texture 0 through the identity texgen
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    Stream list;
    quad(list, {0, 1, 3, 2});
    while (list.bytes.size() % 32) list.u8(0);  // GX pads a display list with NOPs
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    check(pipeline.prepare(gpu, state, {}, 0x90, 0) && pipeline.meshEligible() == smg::GxVertexPipeline::MeshOk, "an unlit, textured state can take the GPU path");
    smg::GxMesh mesh;
    check(pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) == smg::GxVertexPipeline::MeshOk, "a list of primitives decodes");
    check(mesh.vertexCount() == 4 && mesh.indices.size() == 6, "a quad is four vertices and two triangles");
    check(near(mesh.vertex(2).position[0], 640) && near(mesh.vertex(2).position[1], 480), "positions stay in model space");
    check(mesh.layout.stride == 20 && mesh.layout.unit[0].input == smg::GxMeshLayout::Stored && mesh.layout.unit[0].floats == 2 &&
              near(mesh.vertex(2).source[0][0], 1) && near(mesh.vertex(2).source[0][1], 1),
          "a texture coordinate comes along as two floats after the position, with nothing else stored");
    smg::GxMeshUniforms uniforms;
    pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], uniforms);
    const float* m = uniforms.clip[0];
    const float p[3] = {mesh.vertex(0).position[0], mesh.vertex(0).position[1], mesh.vertex(0).position[2]};
    float clip[4];
    for (int r = 0; r < 4; ++r) clip[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
    check(uniforms.slots == 1 && near(clip[0] / clip[3], -1) && near(clip[1] / clip[3], 1), "the draw's matrix puts the top left at (-1, 1)");
    check(uniforms.units == 1 && near(uniforms.texgen[0][0][0], 1) && near(uniforms.texgen[0][1][1], 1) && !uniforms.projective[0],
          "the texgen's rows ride along as uniforms");
    Stream state0;
    state0.bp(0xC008FFF2);
    state0.bytes.insert(state0.bytes.end(), list.bytes.begin(), list.bytes.end());
    check(pipeline.buildMesh(state0.bytes.data(), static_cast<std::uint32_t>(state0.bytes.size()), mesh) == smg::GxVertexPipeline::MeshCommand,
          "a list with state commands stays the CPU's");
}

// A list whose positions are indexed from an array that moves (deformed vertices, double-buffered): the mesh decoded
// again from the new array keeps its triangles and slots and takes the new positions.
void meshRefresh() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);
    s.cp(0x50, 2 << 9), s.cp(0x70, 1 | 4 << 1 | 1 << 21);  // position: an 8-bit index into float XYZ; texture 0 direct
    s.cp(0xA0, 0x100), s.cp(0xB0, 12);
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    const auto place = [](std::uint32_t at, float scale) {
        for (int i = 0; i < 4; ++i) {
            const float p[3] = {scale * (i & 1), scale * (i >> 1), scale};
            std::memcpy(memory.data() + at + 12 * i, p, 12);
        }
    };
    place(0x100, 10), place(0x200, 20);
    Stream list;
    list.u8(0x80), list.u16(4);
    for (unsigned c : {0u, 1u, 3u, 2u}) list.u8(c), list.u8(c & 1), list.u8(c >> 1);
    while (list.bytes.size() % 32) list.u8(0);
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxMesh mesh;
    check(pipeline.prepare(gpu, state, {}, 0x90, 0) &&
              pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) == smg::GxVertexPipeline::MeshOk &&
              near(mesh.vertex(2).position[0], 10) && near(mesh.vertex(2).position[1], 10),
          "a list indexing its positions decodes from the array where it is");
    Stream moved;
    moved.cp(0xA0, 0x200);  // as J3D sets an array before each shape
    gpu.put(moved.bytes.data(), static_cast<std::uint32_t>(moved.bytes.size()));
    const auto refreshed = [&](const std::uint32_t* origin, std::vector<std::uint8_t>& out) {
        out.resize(mesh.vertices.size());
        return pipeline.refreshMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh.layout, origin, mesh.vertices.data(), out.data(),
                                    mesh.vertexCount());
    };
    const auto vertexOf = [&](const std::vector<std::uint8_t>& packed, std::size_t i) {
        return smg::gxUnpackMeshVertex(mesh.layout, packed.data() + i * mesh.layout.stride);
    };
    std::vector<std::uint8_t> out;
    bool same = refreshed(mesh.origin.data(), out);
    for (std::size_t i = 0; same && i < mesh.vertexCount(); ++i)
        same = near(vertexOf(out, i).position[0], 2 * mesh.vertex(i).position[0]) && near(vertexOf(out, i).position[2], 20) &&
               vertexOf(out, i).slot == mesh.vertex(i).slot && near(vertexOf(out, i).source[0][0], mesh.vertex(i).source[0][0]);
    check(same, "decoded again from the moved array, each vertex takes its new position and keeps the rest");
    // A lit mesh's groups copy vertices out of order: each takes the list vertex its origin names.
    const std::vector<std::uint32_t> reversed{3, 2, 1, 0};
    std::vector<std::uint8_t> gathered;
    same = refreshed(reversed.data(), gathered);
    for (std::size_t i = 0; same && i < 4; ++i)
        same = near(vertexOf(gathered, i).position[0], vertexOf(out, 3 - i).position[0]) && near(vertexOf(gathered, i).position[1], vertexOf(out, 3 - i).position[1]);
    check(same, "and out of order, each takes the list vertex its origin names");
}

// A refresh is the build again from the arrays as they are now: every byte of every vertex, the fields the shader
// ignores included, whatever the output held before.
struct RefreshField { unsigned format = 0, count = 0, shift = 0; };  // a VAT entry: GX component format, components, fixed-point shift
struct RefreshFormat {
    RefreshField position, normal, color[2], texcoord[8];
    unsigned nbt = 0;  // normals with binormal and tangent: 1 in one array element, 2 from three indexes
};
struct RefreshCase {
    explicit RefreshCase(const char* what) : what(what) {}
    const char* what;
    bool lit = false, vertexColor = false, matrixIndex = false;
    unsigned texMatrixes = 0;  // texture matrix index bytes in the vertex, a bit per coordinate set
    unsigned type[12] = {};    // the VCD's, for the position, normal, two colors and eight coordinate sets: 0 none, 1 direct, 2 an 8-bit index, 3 a 16-bit one
    std::vector<RefreshFormat> formats;
    struct Texgen { unsigned row; bool three; };
    std::vector<Texgen> texgens;
    struct Primitive { unsigned kind, format, count; };
    std::vector<Primitive> primitives;
};
enum RefreshResult { RefreshNoMesh, RefreshDiffers, RefreshSame };

struct Random {
    std::uint32_t state;
    unsigned next() { return state ^= state << 13, state ^= state >> 17, state ^= state << 5; }
    unsigned below(unsigned n) { return next() % n; }
};
constexpr unsigned ArrayElements = 32, ArraySpan = 0x800, ArrayRegion = 0x8000;
unsigned arrayBase(unsigned array, unsigned version) { return ArrayRegion + (version * 12 + array) * ArraySpan; }
unsigned componentBytes(unsigned format) { return format == 4 ? 4 : format >= 2 ? 2 : 1; }
unsigned colorBytes(unsigned format) { return std::array<unsigned, 8>{2, 3, 4, 2, 3, 4, 4, 4}[format & 7]; }

// One attribute as a vertex stores it: finite floats for GX_F32, random bytes for the other formats and colors.
void attributeBytes(Stream& s, Random& random, unsigned format, unsigned components, bool color) {
    if (color || format != 4) {
        for (unsigned i = 0, n = color ? colorBytes(format) : components * componentBytes(format); i < n; ++i) s.u8(random.next());
        return;
    }
    for (unsigned i = 0; i < components; ++i) s.f32(static_cast<float>(random.below(20000)) / 100 - 100);
}

// What a case sets up: the processor with its formats and arrays, the state and pipeline prepared for it, the list, and
// the mesh built from the arrays where they start.
struct RefreshFixture {
    std::unique_ptr<smg::GxCommandProcessor> gpu = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxDrawState state;
    smg::GxVertexPipeline pipeline;
    std::vector<std::uint8_t> list;
    smg::GxMesh mesh;
    std::uint32_t size() const { return static_cast<std::uint32_t>(list.size()); }
    bool build(const RefreshCase& c, std::uint32_t seed);
    // As J3D sets the arrays before each shape: every array at a new base, with other contents.
    void moveArrays() {
        Stream moved;
        for (unsigned a = 0; a < 12; ++a) moved.cp(0xA0 + a, arrayBase(a, 1));
        gpu->put(moved.bytes.data(), static_cast<std::uint32_t>(moved.bytes.size()));
    }
};

// False when the state or the list takes no mesh.
bool RefreshFixture::build(const RefreshCase& c, std::uint32_t seed) {
    Random random{seed | 1};
    const unsigned units = static_cast<unsigned>(c.texgens.size());
    Stream s;
    clearState(s);
    s.xf(0x103F, {units});
    for (unsigned u = 0; u < units; ++u) s.xf(0x1040 + u, {c.texgens[u].row << 7 | unsigned(c.texgens[u].three) << 2});
    const bool channel = c.vertexColor || c.lit;
    s.bp(units | (channel ? 1u : 0u) << 4 | (units - 1) << 10);  // GEN_MODE: texgens, channels, stages - 1
    s.xf(0x1009, {channel ? 1u : 0u});
    const std::uint32_t control = (c.lit ? 1u << 1 | 1u << 2 | 2u << 7 : 0) | (c.vertexColor ? 1 : 0);
    s.xf(0x100E, {control}), s.xf(0x1010, {control}), s.xf(0x100C, {0xFFFFFFFF}), s.xf(0x100A, {0x10203040});
    s.xff(0x400, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    s.xf(0x600 + 3, {0x80604020}), s.xff(0x600 + 4, {1, 0, 0, 1, 0, 0, 300, 100, 200, 0, 0, 0});
    const auto order = [](unsigned u) { return u | u << 3 | 0x3C0u; };  // texture u through coordinate u, no raster color
    for (unsigned r = 0; 2 * r < units; ++r) s.bp((0x28 + r) << 24 | order(2 * r) | (2 * r + 1 < units ? order(2 * r + 1) << 12 : 0));
    for (unsigned u = 0; u < units; ++u) {  // stage u: its texel, added to the stage before
        s.bp((0xC0 + 2 * u) << 24 | (u ? 0x080FF8 : 0x08FFF8)), s.bp((0xC1 + 2 * u) << 24 | 0x08FF90);
        s.bp((0x88 + u) << 24 | 0x600C03), s.bp((0x94 + u) << 24 | 0x80), s.bp((0x80 + u) << 24 | 0x30);
    }
    std::uint32_t vcdLo = c.matrixIndex | c.texMatrixes << 1 | c.type[0] << 9 | c.type[1] << 11 | c.type[2] << 13 | c.type[3] << 15, vcdHi = 0;
    for (unsigned k = 0; k < 8; ++k) vcdHi |= c.type[4 + k] << 2 * k;
    s.cp(0x50, vcdLo), s.cp(0x60, vcdHi);
    // Each format's VAT, and the bytes of the largest element of each array (its stride a little over).
    unsigned elementBytes[12] = {};
    for (unsigned f = 0; f < c.formats.size(); ++f) {
        const RefreshFormat& v = c.formats[f];
        std::uint32_t vat[3] = {};  // A, B and C as one run of bits
        const auto put = [&](unsigned bit, unsigned value) { vat[bit / 32] |= value << bit % 32; };
        put(0, v.position.count - 2), put(1, v.position.format), put(4, v.position.shift);
        put(9, v.nbt != 0), put(10, v.normal.format), put(31, v.nbt == 2), put(14, v.color[0].format), put(18, v.color[1].format);
        const unsigned at[8][3] = {{21, 22, 25}, {32, 33, 36}, {41, 42, 45}, {50, 51, 54}, {59, 60, 64}, {69, 70, 73}, {78, 79, 82}, {87, 88, 91}};
        for (unsigned k = 0; k < 8; ++k) put(at[k][0], v.texcoord[k].count - 1), put(at[k][1], v.texcoord[k].format), put(at[k][2], v.texcoord[k].shift);
        s.cp(0x70 + f, vat[0]), s.cp(0x80 + f, vat[1]), s.cp(0x90 + f, vat[2]);
        elementBytes[0] = std::max(elementBytes[0], v.position.count * componentBytes(v.position.format));
        elementBytes[1] = std::max(elementBytes[1], 9 * componentBytes(v.normal.format));
        for (unsigned k = 0; k < 2; ++k) elementBytes[2 + k] = std::max(elementBytes[2 + k], colorBytes(v.color[k].format));
        for (unsigned k = 0; k < 8; ++k) elementBytes[4 + k] = std::max(elementBytes[4 + k], v.texcoord[k].count * componentBytes(v.texcoord[k].format));
    }
    unsigned strides[12];
    for (unsigned a = 0; a < 12; ++a) strides[a] = elementBytes[a] + random.below(5), s.cp(0xA0 + a, arrayBase(a, 0)), s.cp(0xB0 + a, strides[a]);
    gpu->put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    // The list: a vertex is its matrix indexes, then its attributes in the layout's order.
    Stream stream;
    for (const RefreshCase::Primitive& p : c.primitives) {
        const RefreshFormat& v = c.formats[p.format];
        stream.u8(p.kind | p.format), stream.u16(p.count);
        for (unsigned i = 0; i < p.count; ++i) {
            if (c.matrixIndex) stream.u8(3 * random.below(10));
            for (unsigned k = 0; k < 8; ++k)
                if (c.texMatrixes >> k & 1) stream.u8(random.below(60));
            const auto attribute = [&](unsigned slot, const RefreshField& f, unsigned components, bool color, unsigned indexes) {
                const unsigned type = c.type[slot];
                if (type == 1) attributeBytes(stream, random, f.format, components, color);
                for (unsigned n = 0; type > 1 && n < indexes; ++n) type == 2 ? stream.u8(random.below(ArrayElements)) : stream.u16(random.below(ArrayElements));
            };
            attribute(0, v.position, v.position.count, false, 1);
            attribute(1, v.normal, v.nbt ? 9 : 3, false, v.nbt == 2 ? 3 : 1);
            attribute(2, v.color[0], 0, true, 1), attribute(3, v.color[1], 0, true, 1);
            for (unsigned k = 0; k < 8; ++k) attribute(4 + k, v.texcoord[k], v.texcoord[k].count, false, 1);
        }
    }
    while (stream.bytes.size() % 32) stream.u8(0);
    list = std::move(stream.bytes);
    for (unsigned a = 0; a < 12; ++a) {  // the elements an index can name, in both versions of each array in use, as floats
        for (unsigned version = 0; version < 2 && c.type[a] > 1; ++version) {
            for (std::uint32_t at = arrayBase(a, version); at < arrayBase(a, version) + ArrayElements * strides[a]; at += 4) {
                const float f = static_cast<float>(random.below(20000)) / 100 - 100;
                std::memcpy(memory.data() + at, &f, 4);
            }
        }
    }
    smg::readGxDrawState(*gpu, state);
    return pipeline.prepare(*gpu, state, {}, 0x90, 0) && state.program.unitCount == units && pipeline.meshEligible() == smg::GxVertexPipeline::MeshOk &&
           pipeline.buildMesh(list.data(), size(), mesh) == smg::GxVertexPipeline::MeshOk;
}

// Fills the stack the next calls will use with garbage, so what a function reads before it writes it shows.
__attribute__((noinline)) void dirtyStack() {
    volatile std::uint32_t pad[0x800];
    for (volatile std::uint32_t& w : pad) w = 0xA5A5A5A5;
}

// The refresh of a built mesh, once its arrays have moved, against the mesh built anew, byte for byte, over an output
// that held garbage.
RefreshResult refreshAgainstRebuild(const RefreshCase& c, std::uint32_t seed) {
    RefreshFixture f;
    if (!f.build(c, seed)) return RefreshNoMesh;
    f.moveArrays();
    smg::GxMesh rebuilt;
    std::vector<std::uint8_t> out(f.mesh.vertices.size(), 0xA5);
    bool same = f.pipeline.buildMesh(f.list.data(), f.size(), rebuilt) == smg::GxVertexPipeline::MeshOk && rebuilt.origin == f.mesh.origin;
    dirtyStack();
    same = same && f.pipeline.refreshMesh(f.list.data(), f.size(), f.mesh.layout, f.mesh.origin.data(), f.mesh.vertices.data(), out.data(), f.mesh.vertexCount()) &&
           out == rebuilt.vertices;
    return same ? RefreshSame : RefreshDiffers;
}

RefreshCase randomRefreshCase(Random& random, const char* what) {
    RefreshCase c{what};
    c.lit = random.below(2), c.vertexColor = random.below(2), c.matrixIndex = random.below(2);
    c.texMatrixes = random.below(4) ? 0 : random.below(32) << 3;  // only sets past the texgens', which take no per-vertex matrix
    const unsigned units = 1 + random.below(3);
    for (unsigned u = 0; u < units; ++u) c.texgens.push_back({random.below(3) ? 5 + random.below(8) : random.below(2), bool(random.below(2))});
    c.type[0] = random.below(16) ? 1 + random.below(3) : 0;
    for (unsigned a = 1; a < 12; ++a) c.type[a] = random.below(3) ? 0 : 1 + random.below(3);
    for (unsigned f = 1 + random.below(2); f > 0; --f) {
        RefreshFormat v;
        v.position = {random.below(5), 2 + random.below(2), random.below(8)}, v.normal = {random.below(5), 3, 0};
        v.nbt = random.below(8) ? 0 : 1 + random.below(2);
        for (RefreshField& color : v.color) color.format = random.below(8);
        for (RefreshField& t : v.texcoord) t = {random.below(5), 1 + random.below(2), random.below(8)};
        c.formats.push_back(v);
    }
    for (unsigned p = 1 + random.below(4); p > 0; --p)
        c.primitives.push_back({std::array<unsigned, 5>{0x80, 0x88, 0x90, 0x98, 0xA0}[random.below(5)], random.below(static_cast<unsigned>(c.formats.size())), 1 + random.below(40)});
    return c;
}

void meshRefreshMatchesRebuild() {
    const auto field = [](unsigned format, unsigned count, unsigned shift = 0) { return RefreshField{format, count, shift}; };
    const auto triangles = [](unsigned format = 0, unsigned count = 24) { return RefreshCase::Primitive{0x90, format, count}; };
    RefreshFormat j3d;  // J3D's usual: float positions, 16-bit normals, a coordinate set of 16-bit fixed point
    j3d.position = field(4, 3), j3d.normal = field(3, 3), j3d.color[0] = field(5, 4), j3d.texcoord[0] = field(3, 2, 8);
    RefreshCase unlit{"an unlit mesh with indexed positions and coordinates"};
    unlit.type[0] = 3, unlit.type[4] = 3, unlit.formats = {j3d}, unlit.texgens = {{5, false}}, unlit.primitives = {{0x98, 0, 30}, triangles()};
    RefreshCase normals = unlit;
    normals.what = "a lit mesh with indexed normals and a texgen from them", normals.type[1] = 3, normals.lit = true, normals.texgens = {{5, false}, {1, true}};
    RefreshCase colored = normals;
    colored.what = "a vertex color the channel takes", colored.type[2] = 2, colored.vertexColor = true, colored.lit = false;
    RefreshCase colored16 = colored;
    colored16.what = "a vertex color by a 16-bit index", colored16.type[2] = 3;
    RefreshCase colorInList = colored;
    colorInList.what = "a vertex color stored in the list", colorInList.type[2] = 1;
    RefreshCase ignored = colored;
    ignored.what = "a vertex color in the format the channel ignores", ignored.vertexColor = false;
    RefreshCase spare = unlit;
    spare.what = "coordinate sets in the format that no texgen reads", spare.type[5] = 2, spare.type[6] = 3, spare.type[7] = 1;
    spare.formats[0].texcoord[1] = field(4, 2), spare.formats[0].texcoord[2] = field(2, 1, 4), spare.formats[0].texcoord[3] = field(1, 2, 3);
    spare.texgens = {{6, false}, {0, false}};  // one reads set 1, past an unread set 0; one reads the position
    RefreshCase direct = unlit;
    direct.what = "attributes stored in the list", direct.type[0] = direct.type[1] = direct.type[4] = 1;
    RefreshCase absent = unlit;
    absent.what = "texgens over coordinate sets the format lacks", absent.texgens = {{9, true}, {5, false}, {1, true}};
    RefreshCase narrow = unlit;
    narrow.what = "formats of one list that differ in components";
    narrow.formats = {j3d, j3d}, narrow.formats[1].position = field(1, 2, 3), narrow.formats[1].texcoord[0] = field(2, 1, 5);
    narrow.texgens = {{0, true}, {5, true}}, narrow.primitives = {triangles(0, 9), triangles(1, 12), {0xA0, 0, 7}, {0x98, 1, 5}, {0x80, 0, 8}};
    RefreshCase skinned = normals;
    skinned.what = "a lit, skinned mesh whose groups copy vertices out of order";
    skinned.matrixIndex = true, skinned.texMatrixes = 0xF0, skinned.primitives = {{0x98, 0, 60}, {0xA0, 0, 20}, triangles(0, 30)};
    for (const RefreshCase* c : {&unlit, &normals, &colored, &colored16, &colorInList, &ignored, &spare, &direct, &absent, &narrow, &skinned})
        for (std::uint32_t seed : {1u, 2u, 3u}) check(refreshAgainstRebuild(*c, seed * 7919) == RefreshSame, c->what);
    // Formats, arrays, texgens and primitives drawn at random.
    Random random{12345};
    unsigned built = 0;
    char what[64];
    for (unsigned i = 0; i < 100; ++i) {
        std::snprintf(what, sizeof what, "random list %u refreshes as it builds", i);
        const RefreshResult result = refreshAgainstRebuild(randomRefreshCase(random, what), random.next());
        check(result != RefreshDiffers, what);
        built += result == RefreshSame;
    }
    check(built > 30, "enough of the random lists make meshes to mean something");
    // A list that no longer gives the mesh its vertices is refused; one that gives more is read as far as the mesh goes.
    RefreshFixture f;
    check(f.build(unlit, 5), "a mesh for the refusals");
    f.moveArrays();
    const std::size_t n = f.mesh.vertexCount(), stride = f.mesh.layout.stride;
    std::vector<std::uint32_t> origin(n + 1);
    for (std::size_t i = 0; i < origin.size(); ++i) origin[i] = static_cast<std::uint32_t>(i);
    std::vector<std::uint8_t> out((n + 1) * stride);
    const auto refresh = [&](std::uint32_t size, const std::uint32_t* from, std::size_t count) {
        std::memset(out.data(), 0xA5, out.size());
        return f.pipeline.refreshMesh(f.list.data(), size, f.mesh.layout, from, f.mesh.vertices.data(), out.data(), count);
    };
    check(!refresh(f.size(), origin.data(), n + 1), "a list with fewer vertices than the mesh is refused");
    check(!refresh(20, origin.data(), n), "and one cut inside a primitive");
    origin[1] = 1000;
    check(!refresh(f.size(), origin.data(), n), "and a gather from a vertex the list lacks");
    origin[1] = 1;
    smg::GxMesh rebuilt;
    check(refresh(f.size(), origin.data(), 5) && f.pipeline.buildMesh(f.list.data(), f.size(), rebuilt) == smg::GxVertexPipeline::MeshOk &&
              !std::memcmp(out.data(), rebuilt.vertices.data(), 5 * stride) && out[5 * stride] == 0xA5,
          "a mesh of fewer vertices takes the list's first ones and leaves the rest of the output");
}

// A pass over the primitive the program just ran takes the program's positions and colors and works out only its own
// coordinates: over the same state, exactly the program's vertices and triangles again.
void passReuse() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);
    s.cp(0x50, 2 << 9), s.cp(0x70, 1 | 4 << 1 | 1 << 21);  // position: an 8-bit index into float XYZ; texture 0 direct
    s.cp(0xA0, 0x100), s.cp(0xB0, 12);
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    for (int i = 0; i < 4; ++i) {
        const float p[3] = {30.0f * (i & 1), 30.0f * (i >> 1), 10};
        std::memcpy(memory.data() + 0x100 + 12 * i, p, 12);
    }
    Stream list;
    for (unsigned c : {0u, 1u, 3u, 2u}) list.u8(c), list.u8(c & 1), list.u8(c >> 1);
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    std::vector<smg::GxDrawVertex> ran(4), again(4);
    std::vector<std::uint16_t> ranIndices(6), againIndices(6);
    std::size_t written = 0;
    const bool prepared = pipeline.prepare(gpu, state, {}, 0x80, 0);
    const std::size_t triangles = pipeline.run(list.bytes.data(), 4, ran.data(), 0, ranIndices.data());
    bool same = prepared && pipeline.runPass(list.bytes.data(), nullptr, 4, ran.data(), again.data(), 0, againIndices.data(), written) &&
                written == triangles && !std::memcmp(ranIndices.data(), againIndices.data(), 2 * triangles);
    for (int i = 0; same && i < 4; ++i) same = !std::memcmp(&ran[i], &again[i], sizeof ran[i]);
    check(same, "a pass over the program's primitive gives its vertices again, coordinates worked out anew");
}

// What gx.v.pica works out for a lit mesh's vertex: the color (0-1).
void shaderLighting(const smg::GxMeshUniforms& u, const smg::GxMeshVertex& v, float out[4]) {
    float clip[4], tex[smg::CombinerProgram::MaxUnits][2];
    smg::gxMeshModel(u, v, clip, out, tex);
}

// A lit model drawn from a mesh: the shader's lighting from the mesh's uniforms has to match the CPU path's, for
// diffuse light from a point and from the SDK's directional 1e18, a spot light, a specular one, and an actor's
// color lit by one light and its alpha by the other.
void litMesh() {
    struct Case {
        const char* what;
        std::uint32_t control, alphaControl;
        float light[2][12];  // A0-A2, K0-K2, position, direction
    };
    const Case cases[] = {
        {"diffuse light from a point and a far direction", 1 << 1 | 3 << 2 | 2 << 7, 1 << 1 | 3 << 2 | 2 << 7,
         {{1, 0, 0, 1, 0, 0, 300, 100, 200, 0, 0, 0}, {1, 0, 0, 1, 0, 0, 0, 0.6e18f, 0.8e18f, 0, 0, 0}}},
        {"a spot light", 1 << 1 | 1 << 2 | 2 << 7 | 1 << 9 | 1 << 10, 1 << 1 | 1 << 2 | 2 << 7 | 1 << 9 | 1 << 10,
         {{0.5f, 0.5f, 0, 1, 0.002f, 0.00001f, 300, 250, 300, 0, 0, -1}, {}}},
        {"a specular light", 1 << 1 | 1 << 2 | 1 << 9, 1 << 1 | 1 << 2 | 1 << 9,
         {{0, 0, 1, 0.5f, 0, 0.5f, 0, -0.4e18f, -0.9e18f, 0, 0.4f, 0.9165f}, {}}},
        {"an actor's color and alpha lit by different lights", 1 << 1 | 1 << 2 | 2 << 7, 1 << 1 | 1 << 3 | 2 << 7,
         {{1, 0, 0, 1, 0, 0, 300, 100, 200, 0, 0, 0}, {1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}}},
        {"color and alpha lit with their own settings", 1 << 1 | 3 << 2 | 2 << 7 | 1 << 9 | 1 << 10, 1 << 1 | 3 << 2 | 1 << 7,
         {{0.5f, 0.5f, 0, 1, 0.002f, 0.00001f, 300, 250, 300, 0, 0, -1}, {1, 0, 0, 1, 0, 0, 0, 0.6e18f, 0.8e18f, 0, 0, 0}}},
    };
    const float vertices[3][6] = {{100, 100, -0.5f, 0, 0, 1}, {500, 100, -0.5f, 0.3f, 0.4f, 0.866f}, {300, 400, -0.5f, -0.6f, 0, 0.8f}};
    for (const Case& test : cases) {
        auto owned = std::make_unique<smg::GxCommandProcessor>(host());
        smg::GxCommandProcessor& gpu = *owned;
        Stream s;
        clearState(s);
        s.cp(0x50, 1 << 9 | 1 << 11), s.cp(0x60, 0), s.cp(0x70, 1 | 4 << 1 | 4 << 10);  // position xyz and normal, f32
        s.xff(0x400, {1, 0, 0, 0, 1, 0, 0, 0, 1});
        s.xf(0x103F, {0}), s.xf(0x1009, {1});
        s.bp(0x00000010), s.bp(0x28000000), s.bp(0xC008FFFA), s.bp(0xC108FFD0);  // one channel; RASC, RASA
        s.xf(0x100C, {0xFFFFFFFF}), s.xf(0x100A, {0x10203040});
        s.xf(0x100E, {test.control}), s.xf(0x1010, {test.alphaControl});
        const std::uint32_t colors[2] = {0x80604020, 0x20406040};  // dim enough that no sum clamps
        for (int n = 0; n < 2; ++n) {
            s.xf(0x600 + 16 * n + 3, {colors[n]});
            const float* l = test.light[n];
            s.xff(0x600 + 16 * n + 4, {l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7], l[8], l[9], l[10], l[11]});
        }
        gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
        Stream list;
        list.u8(0x90), list.u16(3);
        for (const auto& v : vertices)
            for (float f : v) list.f32(f);
        while (list.bytes.size() % 32) list.u8(0);
        smg::GxDraw cpu;
        smg::buildGxDraw(gpu, 0x90, 0, 3, list.bytes.data() + 3, {}, cpu);
        smg::GxDrawState state;
        smg::readGxDrawState(gpu, state);
        smg::GxVertexPipeline pipeline;
        smg::GxMesh mesh;
        smg::GxMeshUniforms u;
        const bool built = pipeline.prepare(gpu, state, {}, 0x90, 0) && pipeline.meshEligible() == smg::GxVertexPipeline::MeshOk &&
                           pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) == smg::GxVertexPipeline::MeshOk;
        bool same = built && cpu.triangles.size() == 3 && mesh.vertexCount() == 3;
        if (same) pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], u);
        for (unsigned i = 0; same && i < 3; ++i) {
            const smg::GxMeshVertex v = mesh.vertex(i);
            float clip[4], color[4], tex[smg::CombinerProgram::MaxUnits][2];
            smg::gxMeshModel(u, v, clip, color, tex);
            const float x = clip[0];
            const smg::GxDrawVertex* match = nullptr;
            for (const auto& d : cpu.triangles)
                if (std::fabs(d.position[0] - x) < 1e-3f) match = &d;
            float shaded[4];
            shaderLighting(u, v, shaded);
            same = match != nullptr;
            for (int c = 0; same && c < 4; ++c) same = std::abs(int(std::lround(shaded[c] * 255)) - int(match->color[c])) <= 2;
        }
        check(same, test.what);
    }
}

// A lit vertex whose color is its ambient (GX_SRC_VTX for the ambient): the mesh carries the color and the shader's
// sums start from it, as the CPU path's do; here the color's ambient comes from the vertex and the alpha's doesn't.
void litMeshAmbientFromVertex() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.cp(0x50, 1 << 9 | 1 << 11 | 1 << 13), s.cp(0x60, 0), s.cp(0x70, 1 | 4 << 1 | 4 << 10 | 1 << 13 | 5 << 14);  // + color, RGBA8
    s.xff(0x400, {1, 0, 0, 0, 1, 0, 0, 0, 1});
    s.xf(0x103F, {0}), s.xf(0x1009, {1});
    s.bp(0x00000010), s.bp(0x28000000), s.bp(0xC008FFFA), s.bp(0xC108FFD0);
    s.xf(0x100C, {0xC0A08060}), s.xf(0x100A, {0x10203040});
    s.xf(0x100E, {1 << 1 | 1 << 2 | 2 << 7 | 1 << 6}), s.xf(0x1010, {1 << 1 | 1 << 2 | 2 << 7});
    s.xf(0x600 + 3, {0x40302010});
    s.xff(0x600 + 4, {1, 0, 0, 1, 0, 0, 300, 100, 200, 0, 0, 0});
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    const float vertices[3][6] = {{100, 100, -0.5f, 0, 0, 1}, {500, 100, -0.5f, 0.3f, 0.4f, 0.866f}, {300, 400, -0.5f, -0.6f, 0, 0.8f}};
    const std::uint8_t colors[3][4] = {{0x20, 0x40, 0x60, 0x80}, {0x50, 0x10, 0x30, 0x70}, {0x05, 0x60, 0x15, 0x30}};
    Stream list;
    list.u8(0x90), list.u16(3);
    for (int i = 0; i < 3; ++i) {
        for (float f : vertices[i]) list.f32(f);
        for (std::uint8_t c : colors[i]) list.u8(c);
    }
    while (list.bytes.size() % 32) list.u8(0);
    smg::GxDraw cpu;
    smg::buildGxDraw(gpu, 0x90, 0, 3, list.bytes.data() + 3, {}, cpu);
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxMesh mesh;
    smg::GxMeshUniforms u;
    const bool built = pipeline.prepare(gpu, state, {}, 0x90, 0) && pipeline.meshEligible() == smg::GxVertexPipeline::MeshOk &&
                       pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) == smg::GxVertexPipeline::MeshOk;
    bool same = built && cpu.triangles.size() == 3 && mesh.vertexCount() == 3;
    if (same) pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], u);
    for (unsigned i = 0; same && i < 3; ++i) {
        float shaded[4];
        shaderLighting(u, mesh.vertex(i), shaded);
        for (int c = 0; same && c < 4; ++c) same = std::abs(int(std::lround(shaded[c] * 255)) - int(cpu.triangles[i].color[c])) <= 2;
    }
    check(same, "a lit mesh whose ambient is its vertex color shades as the CPU path does");
}

// A lit model with ten bones (GX's most): its mesh splits into groups of at most nine slots, and each vertex drawn
// through its group's uniforms lands, and is lit, where the CPU path puts it.
void litMeshGroups() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.cp(0x50, 1 | 1 << 9 | 1 << 11), s.cp(0x60, 0), s.cp(0x70, 1 | 4 << 1 | 4 << 10);  // matrix index, position, normal
    for (unsigned slot = 0; slot < 10; ++slot) {
        const float x = 40.0f * slot, c = std::cos(0.3f * slot), n = std::sin(0.3f * slot);
        s.xff(slot * 12, {c, -n, 0, x, n, c, 0, 10, 0, 0, 1, 0});
        s.xff(0x400 + slot * 9, {c, -n, 0, n, c, 0, 0, 0, 1});
    }
    s.xf(0x103F, {0}), s.xf(0x1009, {1});
    s.bp(0x00000010), s.bp(0x28000000), s.bp(0xC008FFFA), s.bp(0xC108FFD0);
    s.xf(0x100C, {0xFFFFFFFF}), s.xf(0x100A, {0x10203040});
    s.xf(0x100E, {1 << 1 | 1 << 2 | 2 << 7}), s.xf(0x1010, {0});
    s.xf(0x600 + 3, {0x80604020});
    s.xff(0x600 + 4, {1, 0, 0, 1, 0, 0, 300, 100, 200, 0, 0, 0});
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    const unsigned slots[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1};
    Stream list;
    list.u8(0x90), list.u16(12);
    for (unsigned i = 0; i < 12; ++i) {
        list.u8(slots[i] * 3);
        for (float f : {100.0f + 37 * i, 80.0f + 23 * (i % 5), -0.5f, 0.0f, 0.6f, 0.8f}) list.f32(f);
    }
    while (list.bytes.size() % 32) list.u8(0);
    smg::GxDraw cpu;
    smg::buildGxDraw(gpu, 0x90, 0, 12, list.bytes.data() + 3, {}, cpu);
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxMesh mesh;
    check(pipeline.prepare(gpu, state, {}, 0x90, 0) && pipeline.meshEligible() == smg::GxVertexPipeline::MeshOk &&
              pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) == smg::GxVertexPipeline::MeshOk,
          "a lit list with ten bones becomes a mesh");
    check(mesh.groups.size() == 2 && mesh.groups[0].slotCount == 9 && mesh.groups[1].slotCount == 3 && mesh.groups[0].indexCount == 9 &&
              mesh.groups[1].firstIndex == 9 && mesh.groups[1].indexCount == 3,
          "its triangles split into a group of nine slots and one of the rest");
    bool same = cpu.triangles.size() == 12;
    smg::GxMeshUniforms u;
    for (std::size_t g = 0; same && g < mesh.groups.size(); ++g) {
        if (g) pipeline.meshSlotUniforms(mesh.groups[g], u);
        else pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], u);
        for (std::uint32_t i = mesh.groups[g].firstIndex; same && i < mesh.groups[g].firstIndex + mesh.groups[g].indexCount; ++i) {
            const smg::GxMeshVertex v = mesh.vertex(mesh.indices[i]);
            const unsigned k = (static_cast<unsigned>(v.slot) - 4) / 6;
            same = mesh.groups[g].slotMap[k] == slots[i];
            float clip[4], color[4], tex[smg::CombinerProgram::MaxUnits][2];
            smg::gxMeshModel(u, v, clip, color, tex);
            for (int r = 0; same && r < 4; ++r) same = std::fabs(clip[r] - cpu.triangles[i].position[r]) < 1e-3f * std::max(1.0f, std::fabs(clip[r]));
            float shaded[4];
            shaderLighting(u, v, shaded);
            for (int c = 0; same && c < 3; ++c) same = std::abs(int(std::lround(shaded[c] * 255)) - int(cpu.triangles[i].color[c])) <= 2;
        }
    }
    check(same, "each vertex through its group's slot lands and is lit as on the CPU");
}

// A point is a square its size wide in EFB pixels, the texture spread across it where its coordinate takes the point
// offset (JPA's sprites); a line is a band its width wide across its minor axis.
void linesAndPoints() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008FFF8);                                  // color (0, 0, 0, TEXC): texture 0 takes a unit
    s.bp(0x22000000 | 5u << 19 | 60u << 8 | 30u);      // points 10 px with GX_TO_ONE, lines 5 px
    s.bp(0x30000003 | 1u << 19);                       // coordinate 0 takes the point offset
    s.u8(0xB8), s.u16(1), s.u16(320), s.u16(240), s.u8(0), s.u8(0);
    s.u8(0xA8), s.u16(2), s.u16(100), s.u16(100), s.u8(0), s.u8(0), s.u16(300), s.u16(120), s.u8(0), s.u8(0);
    drawn.clear();
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    const auto efb = [](const smg::GxDrawVertex& v, float& x, float& y) {
        x = (v.position[0] / v.position[3] + 1) * 320, y = (1 - v.position[1] / v.position[3]) * 240;
    };
    smg::GxDraw point;
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), {}, point);
    bool square = point.triangles.size() == 6;
    for (const smg::GxDrawVertex& v : point.triangles) {
        float x, y;
        efb(v, x, y);
        square = square && (near(x, 315) || near(x, 325)) && (near(y, 235) || near(y, 245));
        square = square && near(v.texcoord[0][0], x > 320 ? 1 : 0) && near(v.texcoord[0][1], y > 240 ? 1 : 0);
    }
    check(square, "a point is a ten-pixel square, the texture spread over it");
    smg::GxDraw line;
    smg::buildGxDraw(gpu, drawn[1].kind, drawn[1].format, drawn[1].count, drawn[1].data.data(), {}, line);
    bool band = line.triangles.size() == 6;
    for (const smg::GxDrawVertex& v : line.triangles) {
        float x, y;
        efb(v, x, y);
        const float middle = x < 200 ? 100 : 120;
        band = band && (near(x, 100) || near(x, 300)) && (near(y, middle - 2.5f) || near(y, middle + 2.5f)) && near(v.texcoord[0][0], 0);
    }
    check(band, "a line mostly across is a five-pixel band, widened up and down, with no offset where none is taken");
    // The 3DS's frame turned a quarter (as the renderer draws): EFB x is -y / w and EFB y is x / w.
    const smg::GxFrame turned{640, 480, true};
    const auto turnedEfb = [](const smg::GxDrawVertex& v, float& x, float& y) {
        x = (-v.position[1] / v.position[3] + 1) * 320, y = (1 - v.position[0] / v.position[3]) * 240;
    };
    smg::buildGxDraw(gpu, drawn[0].kind, drawn[0].format, drawn[0].count, drawn[0].data.data(), turned, point);
    smg::buildGxDraw(gpu, drawn[1].kind, drawn[1].format, drawn[1].count, drawn[1].data.data(), turned, line);
    bool same = point.triangles.size() == 6 && line.triangles.size() == 6;
    for (const smg::GxDrawVertex& v : point.triangles) {
        float x, y;
        turnedEfb(v, x, y);
        same = same && (near(x, 315) || near(x, 325)) && (near(y, 235) || near(y, 245)) && near(v.texcoord[0][0], x > 320 ? 1 : 0);
    }
    for (const smg::GxDrawVertex& v : line.triangles) {
        float x, y;
        turnedEfb(v, x, y);
        const float middle = x < 200 ? 100 : 120;
        same = same && (near(x, 100) || near(x, 300)) && (near(y, middle - 2.5f) || near(y, middle + 2.5f));
    }
    check(same, "turned a quarter for the 3DS's screen, both land on the same EFB pixels");
}

// J3D's environment maps on a skinned model: a texgen through each vertex's modelview and one through the texture
// matrix paired with its slot, both then through the dual transform. The mesh keeps them, its groups holding fewer
// slots for the paired rows, and each vertex's coordinates come out as the CPU path's.
void litMeshSlotTexgens() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.cp(0x50, 1 | 1 << 1 | 1 << 2 | 1 << 9 | 1 << 11), s.cp(0x60, 0), s.cp(0x70, 1 | 4 << 1 | 4 << 10);  // matrix indices, position, normal
    for (unsigned slot = 0; slot < 10; ++slot) {
        const float x = 40.0f * slot, c = std::cos(0.3f * slot), n = std::sin(0.3f * slot);
        s.xff(slot * 12, {c, -n, 0, x, n, c, 0, 10, 0, 0, 1, 0});
        s.xff(0x400 + slot * 9, {c, -n, 0, n, c, 0, 0, 0, 1});
        s.xff((30 + slot * 3) * 4, {0.5f * c, 0.5f * n, 0, 0.5f, -0.5f * n, 0.5f * c, 0, 0.5f, 0.1f * slot, 0, 0.5f, 1});
    }
    s.xf(0x103F, {2}), s.xf(0x1040, {1 << 1 | 1 << 2}), s.xf(0x1041, {1 << 2 | 1 << 7});  // position (three rows), normal (two)
    s.xf(0x1012, {1}), s.xf(0x1050, {0}), s.xf(0x1051, {3});                             // dual transform: post matrices 0, 1
    s.xff(0x500, {0.01f, 0, 0, 0.5f, 0, 0.01f, 0, 0.25f, 0, 0, 0.001f, 1});
    s.xff(0x50C, {0.5f, 0, 0.2f, 0.5f, 0, 0.5f, 0.3f, 0.5f, 0, 0, 1, 0});
    s.xf(0x1009, {1});
    s.bp(0x00000412), s.bp(0x28049040);                    // two texgens, one channel, two stages: map/coord 0, then 1
    s.bp(0xC008FFF8), s.bp(0xC108FFD0), s.bp(0xC208FFF8), s.bp(0xC308FFD0);
    s.bp(0x89600C03), s.bp(0x95000080), s.bp(0x81000030);  // map 1 as map 0
    s.xf(0x100C, {0xFFFFFFFF}), s.xf(0x100A, {0x10203040});
    s.xf(0x100E, {1 << 1 | 1 << 2 | 2 << 7}), s.xf(0x1010, {0});
    s.xf(0x600 + 3, {0x80604020});
    s.xff(0x600 + 4, {1, 0, 0, 1, 0, 0, 300, 100, 200, 0, 0, 0});
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    const unsigned slots[12] = {0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4};
    Stream list;
    list.u8(0x90), list.u16(12);
    for (unsigned i = 0; i < 12; ++i) {
        list.u8(slots[i] * 3), list.u8(slots[i] * 3), list.u8(30 + slots[i] * 3);
        for (float f : {100.0f + 37 * i, 80.0f + 23 * (i % 5), -0.5f, 0.0f, 0.6f, 0.8f}) list.f32(f);
    }
    while (list.bytes.size() % 32) list.u8(0);
    smg::GxDraw cpu;
    smg::buildGxDraw(gpu, 0x90, 0, 12, list.bytes.data() + 3, {}, cpu);
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxMesh mesh;
    check(pipeline.prepare(gpu, state, {}, 0x90, 0) && pipeline.meshEligible() == smg::GxVertexPipeline::MeshOk &&
              pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) == smg::GxVertexPipeline::MeshOk,
          "a lit, skinned list whose texgens take their slots' matrices becomes a mesh");
    check(mesh.groups.size() == 2 && mesh.groups[0].slotCount == 6 && mesh.groups[0].slotRows == 9 &&
              mesh.groups[0].slotTexgen[0] == smg::GxMesh::SlotModelView && mesh.groups[1].slotTexgen[1] == smg::GxMesh::SlotPaired,
          "its groups hold six slots of nine rows, the modelview and paired texgens marked");
    bool same = cpu.triangles.size() == 12;
    smg::GxMeshUniforms u;
    for (std::size_t g = 0; same && g < mesh.groups.size(); ++g) {
        if (g) pipeline.meshSlotUniforms(mesh.groups[g], u);
        else pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], u);
        for (std::uint32_t i = mesh.groups[g].firstIndex; same && i < mesh.groups[g].firstIndex + mesh.groups[g].indexCount; ++i) {
            const smg::GxMeshVertex v = mesh.vertex(mesh.indices[i]);
            same = mesh.groups[g].slotMap[(static_cast<unsigned>(v.slot) - 4) / 9] == slots[i];
            float clip[4], color[4], tex[smg::CombinerProgram::MaxUnits][2];
            smg::gxMeshModel(u, v, clip, color, tex);
            for (int t = 0; same && t < 2; ++t)
                for (int k = 0; same && k < 2; ++k)
                    same = std::fabs(tex[t][k] - cpu.triangles[i].texcoord[t][k]) < 1e-3f * std::max(1.0f, std::fabs(tex[t][k]));
        }
    }
    check(same, "each vertex's coordinates through its slot's matrices are the CPU path's");
}

// What the shader read of a vertex before meshes were packed: every field, each texgen's input three floats (the third one
// unless it is a position or normal taken with three inputs).
smg::GxMeshVertex fullMeshVertex(const smg::GxVertex& v, bool color, const smg::GxMeshLayout::Texgen* texgens, unsigned units) {
    smg::GxMeshVertex m{};
    std::memcpy(m.position, v.position, sizeof m.position);
    if (color) std::memcpy(m.color, v.color[0], 4);
    for (unsigned u = 0; u < units; ++u) {
        float* in = m.source[u];
        if (texgens[u].source == 0) std::memcpy(in, v.position, 12);
        else if (texgens[u].source == 1) std::memcpy(in, v.normal, 12);
        else in[0] = v.texcoord[texgens[u].source - 5][0], in[1] = v.texcoord[texgens[u].source - 5][1], in[2] = 1;
        if (!texgens[u].threeInputs) in[2] = 1;
    }
    std::memcpy(m.normal, v.normal, sizeof m.normal);
    return m;
}

bool sameBits(const float* a, const float* b, int n) { return !std::memcmp(a, b, n * sizeof(float)); }

// Every layout a mesh can have (color from the vertex or not, lit or not, per-vertex matrices or not, up to three texgens from
// a position, normal or texcoord with two or three inputs): the packed vertex holds exactly the fields the layout names, none
// overlapping and none past its stride, and what the shader reads of it is the full vertex's, bit for bit, the third input of a
// texgen held as two floats made up for in the rows (the sums here are exact, so the rows' outputs match to the bit).
void packedMeshLayouts() {
    smg::GxVertex v;
    const float position[3] = {1.5f, -2.25f, 3.125f}, normal[3] = {0.25f, 0.5f, -0.75f};
    std::memcpy(v.position, position, sizeof position), std::memcpy(v.normal, normal, sizeof normal);
    v.texcoord[0][0] = 0.375f, v.texcoord[0][1] = 0.625f, v.texcoord[3][0] = -5.5f, v.texcoord[3][1] = 7.25f;
    const std::uint8_t color[4] = {1, 2, 3, 4};
    std::memcpy(v.color[0], color, 4);
    const float rows[3][4] = {{0.5f, 0.25f, 0.125f, 0.0625f}, {-0.25f, 0.5f, 0.375f, 0.1875f}, {0.125f, 0, 0.25f, 1}};
    const auto dot4 = [](const float* row, const float* in) { return row[0] * in[0] + row[1] * in[1] + row[2] * in[2] + row[3]; };
    std::vector<smg::GxMeshLayout::Texgen> kinds;
    for (std::uint8_t source : {0, 1, 5, 8})
        for (bool threeInputs : {false, true})
            for (bool slot : {false, true}) kinds.push_back({source, threeInputs, slot});
    unsigned layouts = 0, shortest = 255, longest = 0;
    const std::uint8_t noColor[4] = {};
    for (int colorFromVertex = 0; colorFromVertex < 2; ++colorFromVertex)
        for (int lit = 0; lit < 2; ++lit)
            for (int slot = 0; slot < 2; ++slot)
                for (unsigned units = 0; units <= 3; ++units) {
                    unsigned combinations = 1;
                    for (unsigned u = 0; u < units; ++u) combinations *= static_cast<unsigned>(kinds.size());
                    for (unsigned n = 0; n < combinations; ++n) {
                        smg::GxMeshLayout::Texgen texgens[3] = {};
                        bool eligible = true;
                        for (unsigned u = 0, rest = n; u < units; ++u, rest /= kinds.size()) {
                            texgens[u] = kinds[rest % kinds.size()];
                            eligible &= !texgens[u].perVertexMatrix || (lit && slot);  // a mesh's matrices from the vertex need a lit, skinned one
                        }
                        if (!eligible) continue;
                        ++layouts;
                        const smg::GxMeshLayout layout = smg::GxMeshLayout::make(colorFromVertex, lit, slot, units, texgens);
                        // The stride from the rules, worked out apart from make().
                        unsigned bytes = 12 + 4 * colorFromVertex + 4 * slot;
                        bool normalStored = lit;
                        for (unsigned u = 0; u < units; ++u) {
                            const bool geometry = texgens[u].source <= 1;
                            if (geometry && texgens[u].threeInputs) normalStored |= texgens[u].source == 1;
                            else if (!texgens[u].perVertexMatrix) geometry ? normalStored |= texgens[u].source == 1 : bytes += 8;
                            else bytes += 12;
                        }
                        bytes += normalStored ? 12 : 0;
                        shortest = std::min<unsigned>(shortest, layout.stride), longest = std::max<unsigned>(longest, layout.stride);
                        std::vector<std::uint8_t> packed(layout.stride + 8, 0xA5);
                        smg::GxMeshPacker(layout).pack(&v, 1, packed.data());
                        const float slotRow = 12;
                        if (layout.slot != smg::GxMeshLayout::Absent) std::memcpy(packed.data() + layout.slot, &slotRow, 4);
                        bool guarded = true;
                        for (unsigned g = layout.stride; g < packed.size(); ++g) guarded &= packed[g] == 0xA5;
                        // The fields tile the stride: each byte belongs to one.
                        std::vector<int> owners(layout.stride, 0);
                        const auto claim = [&](unsigned at, unsigned size) {
                            for (unsigned b = at; b < at + size; ++b)
                                if (b < owners.size()) ++owners[b];
                            return at + size <= layout.stride;
                        };
                        bool tiles = claim(0, 12);
                        if (layout.color != smg::GxMeshLayout::Absent) tiles &= claim(layout.color, 4);
                        for (const auto& unit : layout.unit)
                            if (unit.input == smg::GxMeshLayout::Stored) tiles &= claim(unit.at, 4u * unit.floats);
                        if (layout.normal != smg::GxMeshLayout::Absent) tiles &= claim(layout.normal, 12);
                        if (layout.slot != smg::GxMeshLayout::Absent) tiles &= claim(layout.slot, 4);
                        for (int owner : owners) tiles &= owner == 1;
                        const smg::GxMeshVertex got = smg::gxUnpackMeshVertex(layout, packed.data());
                        const smg::GxMeshVertex want = fullMeshVertex(v, colorFromVertex, texgens, units);
                        bool same = sameBits(got.position, want.position, 3) && !std::memcmp(got.color, colorFromVertex ? color : noColor, 4);
                        same &= normalStored ? sameBits(got.normal, want.normal, 3) : layout.normal == smg::GxMeshLayout::Absent;
                        same &= got.slot == (slot ? slotRow : lit ? 4.0f : 0.0f);
                        for (unsigned u = 0; u < 3; ++u) {
                            const auto& unit = layout.unit[u];
                            if (u >= units) {
                                same &= unit.input == smg::GxMeshLayout::Unused;
                                continue;
                            }
                            same &= unit.input != smg::GxMeshLayout::Unused && unit.floats >= 2 && sameBits(got.source[u], want.source[u], unit.floats);
                            same &= unit.floats == 3 || (got.source[u][2] == 0 && want.source[u][2] == 1);
                            // The shader's rows over what it reads match the full vertex's rows over its inputs.
                            float folded[3][4];
                            std::memcpy(folded, rows, sizeof folded);
                            if (unit.floats == 2)
                                for (auto& row : folded) row[3] += row[2], row[2] = 0;
                            for (int r = 0; r < 3; ++r) same &= dot4(folded[r], got.source[u]) == dot4(rows[r], want.source[u]);
                            // A two-input texgen of a slot reads all three, so it keeps them.
                            same &= !texgens[u].perVertexMatrix || unit.floats == 3;
                        }
                        if (!(guarded && tiles && same && layout.stride == bytes && layout.stride % 4 == 0)) {
                            std::printf("layout: color %d lit %d slot %d units %u combination %u stride %u want %u guarded %d tiles %d same %d\n", colorFromVertex,
                                        lit, slot, units, n, layout.stride, bytes, guarded, tiles, same);
                            check(false, "a layout packs what it names and the shader reads the full vertex's values");
                        }
                    }
                }
    check(layouts > 12000 && shortest == 12 && longest <= 68, "every combination of layout packs its vertex whole, bit for bit, from the bare position up");
    smg::GxMeshLayout::Texgen texcoord = {5, false, false};
    const smg::GxMeshLayout::Texgen both[2] = {texcoord, {1, true, false}};
    const smg::GxMeshLayout lit = smg::GxMeshLayout::make(true, true, false, 2, both);
    check(lit.stride == 36 && lit.unit[0].input == smg::GxMeshLayout::Stored && lit.unit[0].floats == 2 && lit.unit[1].input == smg::GxMeshLayout::OfNormal,
          "a lit, vertex-colored model with a texture and an environment map takes 36 bytes of 68");
    check(smg::GxMeshLayout::make(false, false, false, 1, &texcoord).stride == 20 && smg::GxMeshLayout::make(false, false, false, 0, nullptr).stride == 12,
          "an unlit texture is 20 bytes, a bare position 12");
    smg::GxMeshLayout::Texgen other = texcoord;
    other.source = 6;
    check(smg::GxMeshLayout::make(false, true, false, 1, &texcoord).sameBuffers(smg::GxMeshLayout::make(false, true, false, 1, &other)) &&
              smg::GxMeshLayout::make(false, true, false, 1, &texcoord) != smg::GxMeshLayout::make(false, true, false, 1, &other) &&
              !lit.sameBuffers(smg::GxMeshLayout::make(true, true, false, 1, &texcoord)),
          "layouts alike but for the texcoord a stored input comes from share their loaders");
    check(!std::strcmp(lit.fieldAt(0), "position") && !std::strcmp(lit.fieldAt(lit.color), "color") && !std::strcmp(lit.fieldAt(lit.unit[0].at + 4), "texgen inputs") &&
              !std::strcmp(lit.fieldAt(lit.normal + 11), "normal"),
          "a byte of a vertex names its field for the logs");
}

// One texgen unit of a generated mesh state: its XF source row (0 position, 1 normal, 5 texcoord 0), the input form, whether
// its matrix comes from the vertex (a lit, skinned mesh's), and how many rows its matrix has.
struct GeneratedUnit {
    unsigned row;
    bool threeInputs, slot;
    unsigned rows;
};
struct GeneratedMesh {
    bool lit, skinned;
    unsigned colorFrom;  // 0 the register, 1 the vertex's material, 2 (lit) the vertex's ambient
    std::vector<GeneratedUnit> units;
};

// The first way a mesh made under `c` differs from the CPU path's vertices (or null): its layout against the rules, each vertex's
// position, color and coordinates through its group's uniforms, and a refresh of the same list giving the same bytes.
const char* generatedMeshDiffers(const GeneratedMesh& c, smg::GxMeshLayout& layoutOut) {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    const unsigned units = static_cast<unsigned>(c.units.size()), stages = std::max(units, 1u), channels = c.lit || c.colorFrom ? 1 : 0;
    unsigned vcd = 1 << 9 | 1 << 11 | 1 << 13 | (c.skinned ? 1 : 0);
    for (unsigned u = 0; u < units; ++u) vcd |= (c.units[u].slot ? 1u : 0u) << (1 + u);
    s.cp(0x50, vcd), s.cp(0x60, 1), s.cp(0x70, 1 | 4 << 1 | 4 << 10 | 1 << 13 | 5 << 14 | 1 << 21 | 4 << 22);
    for (unsigned slot = 0; slot < 10; ++slot) {
        const float x = 40.0f * slot, cs = std::cos(0.3f * slot), sn = std::sin(0.3f * slot);
        s.xff(slot * 12, {cs, -sn, 0, x, sn, cs, 0, 10, 0, 0, 1, 0});
        s.xff(0x400 + slot * 9, {cs, -sn, 0, sn, cs, 0, 0, 0, 1});
    }
    std::uint32_t matrices = 0;
    for (unsigned u = 0; u < units; ++u) {
        matrices |= (60 + 3 * u) << (6 + 6 * u);
        s.xff((60 + 3 * u) * 4, {0.5f, 0.25f, 0.125f + 0.0625f * u, 0.0625f, -0.25f, 0.5f, 0.375f, 0.1875f, 0.125f, 0, 0.25f, 1});
        s.xf(0x1040 + u, {(c.units[u].rows == 3 ? 1u << 1 : 0u) | (c.units[u].threeInputs ? 1u << 2 : 0u) | c.units[u].row << 7});
    }
    s.xf(0x1018, {matrices}), s.xf(0x1009, {channels}), s.xf(0x103F, {units}), s.xf(0x1012, {0});
    s.bp(units | channels << 4 | (stages - 1) << 10);
    // Two stages share each order register: map and coord k with its texture on, else no texture; no channel without lighting or color.
    const auto place = [&](unsigned k) -> std::uint32_t {
        if (k >= stages) return 0;
        return (k < units ? (k | k << 3 | 1u << 6) : 0u) | (channels ? 0u : 7u) << 7;
    };
    for (unsigned pair = 0; pair < (stages + 1) / 2; ++pair) s.bp((0x28u + pair) << 24 | place(2 * pair + 1) << 12 | place(2 * pair));
    for (unsigned k = 0; k < stages; ++k) {
        s.bp(0xC0000000u + 2 * k * 0x01000000u + (k < units ? 0x08FFF8u : 0x08FFFAu)), s.bp(0xC1000000u + 2 * k * 0x01000000u + 0x08FFD0u);
        if (k < 3) s.bp(0x88600C03 + k * 0x01000000u), s.bp(0x94000080 + k * 0x01000000u), s.bp(0x80000030 + k * 0x01000000u);
    }
    s.xf(0x100C, {0xFFFFFFFF}), s.xf(0x100A, {0x10203040});
    const std::uint32_t light = c.lit ? 1 << 1 | 1 << 2 | 2 << 7 : 0;
    s.xf(0x100E, {light | (c.colorFrom == 1 ? 1u : 0u) | (c.colorFrom == 2 ? 1u << 6 : 0u)}), s.xf(0x1010, {light});
    s.xf(0x600 + 3, {0x80604020});
    s.xff(0x600 + 4, {1, 0, 0, 1, 0, 0, 300, 100, 200, 0, 0, 0});
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    Stream list;
    list.u8(0x90), list.u16(6);
    for (unsigned i = 0; i < 6; ++i) {
        const unsigned slot = i % 4;
        if (c.skinned) list.u8(slot * 3);
        for (const GeneratedUnit& u : c.units)
            if (u.slot) list.u8(slot * 3);
        for (float f : {100.0f + 37 * i, 80.0f + 23 * (i % 5), -0.5f, 0.0f, 0.6f, 0.8f}) list.f32(f);
        for (int k = 0; k < 4; ++k) list.u8(0x20 + 0x13 * i + 7 * k);
        list.f32(0.125f * i), list.f32(0.5f - 0.0625f * i);
    }
    while (list.bytes.size() % 32) list.u8(0);
    smg::GxDraw cpu;
    smg::buildGxDraw(gpu, 0x90, 0, 6, list.bytes.data() + 3, {}, cpu);
    smg::GxDrawState state;
    smg::readGxDrawState(gpu, state);
    smg::GxVertexPipeline pipeline;
    smg::GxMesh mesh;
    if (!pipeline.prepare(gpu, state, {}, 0x90, 0) || pipeline.meshEligible() != smg::GxVertexPipeline::MeshOk) return "the state is not a mesh's";
    if (pipeline.buildMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh) != smg::GxVertexPipeline::MeshOk) return "the list does not build";
    layoutOut = mesh.layout;
    if (cpu.triangles.size() != 6 || mesh.vertexCount() < 6 || state.program.unitCount != units) return "the CPU path or the program is not as set";
    if (mesh.layout != pipeline.meshLayout()) return "the mesh's layout is not the state's";
    std::vector<smg::GxMeshLayout::Texgen> texgens;
    for (const GeneratedUnit& u : c.units) texgens.push_back({static_cast<std::uint8_t>(u.row), u.threeInputs, u.slot});
    if (mesh.layout != smg::GxMeshLayout::make(c.colorFrom != 0, c.lit, c.skinned, units, texgens.data())) return "the layout is not the rules'";
    smg::GxMeshUniforms u;
    const auto off = [](float got, float want) { return std::fabs(got - want) > 1e-3f * std::max(1.0f, std::fabs(want)); };
    for (std::size_t g = 0; g < mesh.groups.size(); ++g) {
        if (g) pipeline.meshSlotUniforms(mesh.groups[g], u);
        else pipeline.meshUniforms(mesh.layout, mesh.slots, mesh.groups[0], u);
        for (std::uint32_t i = mesh.groups[g].firstIndex; i < mesh.groups[g].firstIndex + mesh.groups[g].indexCount; ++i) {
            float clip[4], color[4], tex[smg::CombinerProgram::MaxUnits][2];
            smg::gxMeshModel(u, mesh.vertex(mesh.indices[i]), clip, color, tex);
            const smg::GxDrawVertex& want = cpu.triangles[i];
            for (int r = 0; r < 4; ++r)
                if (std::fabs(clip[r] - want.position[r]) > 1e-3f * std::max({1.0f, std::fabs(want.position[3]), std::fabs(want.position[r])})) return "a position";
            for (int k = 0; k < 4; ++k)
                if (std::abs(int(std::lround(color[k] * 255)) - int(want.color[k])) > 2) return "a color";
            for (unsigned t = 0; t < units; ++t)
                if (off(tex[t][0], want.texcoord[t][0]) || off(tex[t][1], want.texcoord[t][1])) return "a texture coordinate";
        }
    }
    std::vector<std::uint8_t> again(mesh.vertices.size());
    if (!pipeline.refreshMesh(list.bytes.data(), static_cast<std::uint32_t>(list.bytes.size()), mesh.layout, mesh.origin.data(), mesh.vertices.data(), again.data(),
                              mesh.vertexCount()) ||
        again != mesh.vertices)
        return "a refresh of the same list";
    return nullptr;
}

// Meshes under generated states, every lighting and color source, skinned or not, and up to three texgens of each kind: laid out
// by the rules, they put every vertex where the CPU path does, with its color and coordinates, and refresh to the same bytes.
void packedMeshesMatchTheCpuPath() {
    const GeneratedUnit kinds[] = {{0, true, false, 3}, {0, false, false, 2}, {1, true, false, 2}, {1, false, false, 2}, {5, false, false, 2},
                                   {5, true, false, 3}, {0, true, true, 3}, {1, true, true, 2}, {1, false, true, 2}, {5, false, true, 2}};
    unsigned cases = 0, fits = 0;
    const char* why = nullptr;
    for (int lit = 0; lit < 2 && !why; ++lit)
        for (int skinned = 0; skinned < 2 && !why; ++skinned)
            for (unsigned colorFrom = 0; colorFrom < 3 && !why; ++colorFrom) {
                if (colorFrom == 2 && !lit) continue;
                for (unsigned count = 0; count <= 3 && !why; ++count) {
                    unsigned combinations = 1;
                    for (unsigned k = 0; k < count; ++k) combinations *= 10;
                    for (unsigned n = 0; n < combinations && !why; ++n) {
                        GeneratedMesh c{bool(lit), bool(skinned), colorFrom, {}};
                        bool eligible = true;
                        for (unsigned k = 0, rest = n; k < count; ++k, rest /= 10) {
                            c.units.push_back(kinds[rest % 10]);
                            eligible &= !c.units.back().slot || (lit && skinned);
                        }
                        if (!eligible || (count == 3 && n % 7)) continue;  // all of one and two units, a seventh of three
                        smg::GxMeshLayout layout;
                        why = generatedMeshDiffers(c, layout);
                        ++cases, fits += layout.stride <= 68;
                        if (why) std::printf("generated mesh: lit %d skinned %d color %u units %u combination %u: %s\n", lit, skinned, colorFrom, count, n, why);
                    }
                }
            }
    check(!why, "a mesh under every generated state matches the CPU path, packed, and refreshes to the same bytes");
    check(cases > 300 && fits == cases, "and none of them takes more than GxMeshVertex's 68 bytes");
}
} // namespace

// nw4r's layout material: black (C0) to white (C1) by the texture, which is the texture itself. The material keeps
// C0, C1, C2, PREV, as the combiner reads them; BP 0xE0 is PREV's, so a gray there must not become C0.
void layoutMaterial() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0xC008248F);                                    // color: lerp(C0, C1, TEXC), clamped
    s.bp(0xE0080080), s.bp(0xE1080080);                  // PREV = gray
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

// The draw-state key holds what a state reads and nothing it doesn't: an unused stage or texture map changes nothing,
// and the same key means the same state.
void drawStateKey() {
    auto owned = std::make_unique<smg::GxCommandProcessor>(host());
    smg::GxCommandProcessor& gpu = *owned;
    Stream s;
    clearState(s);
    s.bp(0x88000000 | 0x100);  // map 0's size and format
    gpu.put(s.bytes.data(), static_cast<std::uint32_t>(s.bytes.size()));
    std::uint32_t base[smg::MaxDrawStateKey], key[smg::MaxDrawStateKey];
    const std::size_t baseCount = smg::gxDrawStateKey(gpu, base);
    const auto same = [&] {
        const std::size_t count = smg::gxDrawStateKey(gpu, key);
        return count == baseCount && !std::memcmp(key, base, count * 4);
    };
    Stream unused;
    unused.bp(0xCA08FFAA), unused.bp(0x2B0003C0), unused.bp(0x8B000123), unused.bp(0xAB000321);  // stage 5, orders 6-7, maps 3 and 7
    gpu.put(unused.bytes.data(), static_cast<std::uint32_t>(unused.bytes.size()));
    check(same() && baseCount < smg::MaxDrawStateKey, "an unused stage or texture map leaves the key as it was");
    smg::GxDrawState a, b;
    smg::readGxDrawState(gpu, a);
    Stream used;
    used.bp(0x94000200);  // map 0's image address: a map stage 0 reads
    gpu.put(used.bytes.data(), static_cast<std::uint32_t>(used.bytes.size()));
    check(!same(), "a used texture map changes the key");
    smg::readGxDrawState(gpu, b);
    check(a.textures[0].address != b.textures[0].address, "and the state read with it");
    Stream stage;
    stage.bp(0x94000100), stage.bp(0xC008FFF3);  // back to the first map; stage 0's color inputs changed
    gpu.put(stage.bytes.data(), static_cast<std::uint32_t>(stage.bytes.size()));
    check(!same(), "a used stage changes the key");
    Stream readsC1;
    readsC1.bp(0xC008FFF4);  // stage 0's color is C1 alone
    gpu.put(readsC1.bytes.data(), static_cast<std::uint32_t>(readsC1.bytes.size()));
    const std::size_t c1Count = smg::gxDrawStateKey(gpu, base);
    const auto sameAsC1 = [&] {
        const std::size_t count = smg::gxDrawStateKey(gpu, key);
        return count == c1Count && !std::memcmp(key, base, count * 4);
    };
    Stream others;
    others.bp(0xE6012345), others.bp(0xE7054321), others.bp(0xE0800123);  // C2, and K0's red: no stage reads them
    gpu.put(others.bytes.data(), static_cast<std::uint32_t>(others.bytes.size()));
    check(sameAsC1(), "a TEV register or konst no stage reads leaves the key as it was");
    Stream c1;
    c1.bp(0xE4000077);  // C1's red
    gpu.put(c1.bytes.data(), static_cast<std::uint32_t>(c1.bytes.size()));
    check(!sameAsC1(), "a register a stage reads changes it");
    const auto keyOf = [&](std::uint32_t* out, bool palette, unsigned* markers) { return smg::gxDrawStateKey(gpu, out, palette, markers); };
    const auto put = [&](std::initializer_list<std::uint32_t> loads) {
        Stream w;
        for (std::uint32_t load : loads) w.bp(load);
        gpu.put(w.bytes.data(), static_cast<std::uint32_t>(w.bytes.size()));
    };
    put({0xF6000004, 0xF700000E});  // swap table 0 is the identity
    unsigned markers = 0;
    std::size_t before = keyOf(base, true, &markers);
    put({0xE4000033});  // another C1
    check((markers & 2) && keyOf(key, true, nullptr) == before && !std::memcmp(key, base, before * 4), "with the palette, a C1 that rides leaves the key as it was");
    put({0xC008FFF2});  // stage 0's color is C0 alone
    before = keyOf(base, false, &markers);
    put({0xE2000044});  // another C0
    check((markers & 1) && keyOf(key, false, nullptr) == before && !std::memcmp(key, base, before * 4), "so does a C0 that rides in the vertex color");
    put({0xE2000600});  // C0's red below zero: no vertex color holds it
    const std::size_t after = keyOf(key, false, &markers);
    check(!(markers & 1) && (after != before || std::memcmp(key, base, after * 4)), "and one that can't changes it");
}

int runGxDrawTests() {
    tevOpTables();
    clearQuad();
    indirect();
    lighting();
    layoutMaterial();
    texturedQuad();
    perspective();
    channels();
    pipelineAcrossPrimitives();
    dualTexgen();
    atlasPlace();
    color0Rides();
    color1Rides();
    decodedVertices();
    meshSpans();
    meshFromDisplayList();
    litMesh();
    litMeshGroups();
    litMeshAmbientFromVertex();
    litMeshSlotTexgens();
    packedMeshLayouts();
    packedMeshesMatchTheCpuPath();
    linesAndPoints();
    meshRefresh();
    meshRefreshMatchesRebuild();
    passReuse();
    drawStateKey();
    std::printf("%u GX draw checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runGxDrawTests(); }
#endif
