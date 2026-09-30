#include <smg/Model.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

using Data = std::vector<std::uint8_t>;
void put16(Data& b, std::size_t at, unsigned v) { b[at] = v >> 8; b[at + 1] = v; }
void put32(Data& b, std::size_t at, std::uint32_t v) { put16(b, at, v >> 16); put16(b, at + 2, v); }
void putFloat(Data& b, std::size_t at, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, 4);
    put32(b, at, bits);
}
Data block(const char* name, unsigned size) {
    Data b(size, 0);
    std::memcpy(b.data(), name, 4);
    put32(b, 4, size);
    return b;
}

struct Fixture {
    Data data;
    std::size_t shape, vertex, material, texture;
};

struct Options {
    unsigned command = 0x98;
    bool twoJoints = false;   // joint 1, a child of joint 0, moved by (10, 0, 0) and turned 90 degrees about Y
    bool envelope = false;    // the shape uses an even blend of both joints instead
};

// One shape and one material drawing a unit quad with one primitive command.
Fixture fixture(Options options = {}) {
    const unsigned joints = options.twoJoints || options.envelope ? 2 : 1;
    auto inf = block("INF1", 64);
    put32(inf, 20, 24);
    std::vector<std::pair<unsigned, unsigned>> hierarchy{{0x10, 0}, {1, 0}};
    if (joints == 2) hierarchy.insert(hierarchy.end(), {{0x10, 1}, {1, 0}});
    hierarchy.insert(hierarchy.end(), {{0x11, 0}, {1, 0}, {0x12, 0}, {2, 0}, {2, 0}});
    if (joints == 2) hierarchy.push_back({2, 0});
    hierarchy.push_back({0, 0});
    inf.resize(24 + hierarchy.size() * 4);
    put32(inf, 4, inf.size());
    for (std::size_t i = 0; i < hierarchy.size(); ++i) {
        put16(inf, 24 + i * 4, hierarchy[i].first);
        put16(inf, 26 + i * 4, hierarchy[i].second);
    }

    auto vtx = block("VTX1", 144);
    put32(vtx, 8, 64);
    put32(vtx, 12, 96);
    put32(vtx, 64, 9);
    put32(vtx, 68, 1);
    put32(vtx, 72, 4);
    put32(vtx, 80, 255);
    const float positions[]{-1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0};
    for (unsigned i = 0; i < 12; ++i) putFloat(vtx, 96 + i * 4, positions[i]);

    auto drw = block("DRW1", 32);
    const unsigned drawCount = options.envelope ? 1 : joints;
    put16(drw, 8, drawCount);
    put32(drw, 12, 20);
    put32(drw, 16, 24);
    if (options.envelope) drw[20] = 1;
    else if (joints == 2) put16(drw, 26, 1);

    auto jnt = block("JNT1", 24 + joints * 64 + 4);
    put16(jnt, 8, joints);
    put32(jnt, 12, 24);
    put32(jnt, 16, 24 + joints * 64);
    for (unsigned j = 0; j < joints; ++j) {
        for (unsigned i = 0; i < 3; ++i) putFloat(jnt, 24 + j * 64 + 4 + i * 4, 1);
        put16(jnt, 24 + joints * 64 + j * 2, j);
    }
    if (joints == 2) {
        put16(jnt, 24 + 64 + 18, 0x4000);
        putFloat(jnt, 24 + 64 + 24, 10);
    }

    auto shp = block("SHP1", 131);
    put16(shp, 8, 1);
    put32(shp, 12, 44);
    put32(shp, 16, 84);
    put32(shp, 24, 86);
    put32(shp, 28, 102);
    put32(shp, 32, 104);
    put32(shp, 36, 115);
    put32(shp, 40, 123);
    put16(shp, 46, 1);
    put32(shp, 86, 9);
    put32(shp, 90, 3);
    put32(shp, 94, 255);
    const unsigned count = options.command == 0x90 ? 3 : 4;
    shp[104] = options.command;
    put16(shp, 105, count);
    for (unsigned i = 0; i < count; ++i) put16(shp, 107 + i * 2, i);
    if (options.command == 0xa0 || options.command == 0x80) {
        put16(shp, 111, 3);
        put16(shp, 113, 2);
    }
    // The shape's current matrix: the child joint when there is one.
    put16(shp, 115, joints == 2 && !options.envelope ? 1 : 0);
    put16(shp, 117, 1);
    put16(shp, 102, joints == 2 && !options.envelope ? 1 : 0);
    put32(shp, 123, 3 + count * 2);

    auto mat = block("MAT3", 475);
    put16(mat, 8, 1);
    put32(mat, 12, 132);
    put32(mat, 16, 464);
    put32(mat, 28, 466);
    put32(mat, 32, 470);
    put32(mat, 88, 474);
    put16(mat, 132 + 72, 65535);
    put16(mat, 132 + 132, 65535);
    put32(mat, 466, 2);
    put32(mat, 470, 0xffffffff);
    mat[474] = 1;

    auto tex = block("TEX1", 20);
    put32(tex, 12, 20);

    Fixture f;
    f.data.assign(32, 0);
    std::memcpy(f.data.data(), "J3D2bmd3", 8);
    put32(f.data, 12, options.envelope ? 8 : 7);
    const auto append = [&](const Data& b) {
        const auto offset = f.data.size();
        f.data.insert(f.data.end(), b.begin(), b.end());
        return offset;
    };
    append(inf);
    f.vertex = append(vtx);
    if (options.envelope) {
        // Envelope 0 mixes joints 0 and 1 half and half, with identity inverse binds.
        auto evp = block("EVP1", 28 + 4 + 4 + 8 + 96);
        put16(evp, 8, 1);
        put32(evp, 12, 28);
        put32(evp, 16, 32);
        put32(evp, 20, 36);
        put32(evp, 24, 44);
        evp[28] = 2;
        put16(evp, 32, 0);
        put16(evp, 34, 1);
        putFloat(evp, 36, 0.5f);
        putFloat(evp, 40, 0.5f);
        for (unsigned j = 0; j < 2; ++j) for (unsigned d = 0; d < 3; ++d) putFloat(evp, 44 + j * 48 + d * 20, 1);
        append(evp);
    }
    append(drw);
    append(jnt);
    f.shape = append(shp);
    f.material = append(mat);
    f.texture = append(tex);
    put32(f.data, 8, f.data.size());
    return f;
}

bool load(const Data& data, smg::Model& model, std::size_t budget, std::string& error) {
    return smg::loadModel({data.data(), data.size()}, model, budget, error);
}

void models() {
    smg::Model model;
    std::string error;
    auto f = fixture();
    check(load(f.data, model, 4096, error), "minimal original-format model decoded");
    check(model.vertices.size() == 6 && model.shapes.size() == 1 && model.materials.size() == 1, "strip triangulation counts");
    for (unsigned i = 0; i < 6; i += 3) {
        const auto *a = model.vertices[i].position, *b = model.vertices[i + 1].position, *c = model.vertices[i + 2].position;
        check((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) > 0, "strip winding preserved on alternating triangles");
    }
    check(model.minimum[0] == -1 && model.maximum[1] == 1, "decoded model bounds");
    check(model.materials[0].cullMode == 2 && model.materials[0].textures[0] == -1 &&
          near(model.materials[0].materialColor[0][3], 1), "material defaults and indexed tables");
    for (unsigned primitive : {0x90u, 0xa0u, 0x80u}) {
        const auto primitiveModel = fixture({primitive});
        check(load(primitiveModel.data, model, 4096, error), "triangle, fan and quad commands");
        check(model.vertices.size() == (primitive == 0x90 ? 3 : 6), "primitive triangle count");
    }

    Options child;
    child.twoJoints = true;
    check(load(fixture(child).data, model, 4096, error), "two-joint model decoded");
    check(model.joints.size() == 2 && model.joints[1].parent == 0, "joint hierarchy from INF1");
    // A quarter turn about Y sends local +X to -Z before the child's translation.
    const auto& moved = model.vertices[0].position;
    check(near(moved[0], 10) && near(moved[1], -1) && near(moved[2], 1), "rigid vertices use their joint's bind matrix");
    Options skinned;
    skinned.envelope = true;
    check(load(fixture(skinned).data, model, 4096, error), "enveloped model decoded");
    check(near(model.vertices[0].position[0], 4.5f) && near(model.vertices[0].position[2], 0.5f), "envelope weights blend joint matrices");

    for (std::size_t n = 0; n < f.data.size(); ++n)
        check(!smg::loadModel({f.data.data(), n}, model, 4096, error), "truncated model rejected");
    check(!load(f.data, model, 100, error), "geometry budget enforced");

    auto bad = f.data;
    put16(bad, f.shape + 107, 100);
    check(!load(bad, model, 4096, error), "out-of-range vertex index rejected");
    bad = f.data;
    put32(bad, f.shape + 127, 0xfffffff0);
    check(!load(bad, model, 4096, error), "overflowing display-list offset rejected");
    bad = f.data;
    put32(bad, f.material + 12, 0xffffffff);
    check(!load(bad, model, 4096, error), "material offset rejected before arithmetic");
    bad = f.data;
    putFloat(bad, f.vertex + 96, NAN);
    check(!load(bad, model, 4096, error), "nonfinite positions rejected");
    bad = f.data;
    bad[f.shape + 104] = 0x61;
    check(!load(bad, model, 4096, error), "GX register writes cannot masquerade as geometry");
    bad = f.data;
    put16(bad, f.shape + 115, 5);
    check(!load(bad, model, 4096, error), "shape draw matrix out of range rejected");
}

smg::Image image(smg::GxFormat format, unsigned width, unsigned height, const Data& data,
                 const Data& palette = {}, smg::GxPalette paletteFormat = smg::GxPalette::RGB565) {
    smg::Image result;
    std::string error;
    check(smg::decodeGxImage({data.data(), data.size()}, format, width, height,
                             {palette.data(), palette.size()}, paletteFormat, result, error), "GX image decodes");
    return result;
}

void textures() {
    using smg::GxFormat;
    Data cmpr(32, 0);
    const unsigned endpoints[]{0xf800, 0x07e0, 0x001f, 0xffff};
    for (unsigned i = 0; i < 4; ++i) put16(cmpr, i * 8, endpoints[i]);
    auto texture = image(GxFormat::CMPR, 8, 8, cmpr);
    check(texture.pixels[0] == 0xff0000ff && texture.pixels[4] == 0x00ff00ff &&
          texture.pixels[32] == 0x0000ffff && texture.pixels[36] == 0xffffffff, "CMPR sub-block order and RGB565 expansion");
    put16(cmpr, 0, 0xf800);
    put16(cmpr, 2, 0x07e0);
    cmpr[4] = 0x1b;
    texture = image(GxFormat::CMPR, 8, 8, cmpr);
    check(texture.pixels[0] == 0xff0000ff && texture.pixels[1] == 0x00ff00ff &&
          texture.pixels[2] == 0x9f5f00ff && texture.pixels[3] == 0x5f9f00ff, "CMPR selector bit order and eighths interpolation");
    put16(cmpr, 0, 0);
    put16(cmpr, 2, 0xffff);
    cmpr[4] = 0xff;
    texture = image(GxFormat::CMPR, 8, 8, cmpr);
    check(texture.pixels[0] == 0x7f7f7f00, "CMPR transparent mode keeps the averaged color");
    smg::Image rejected;
    std::string error;
    check(!smg::decodeGxImage({cmpr.data(), 31}, GxFormat::CMPR, 8, 8, {}, smg::GxPalette::RGB565, rejected, error),
          "truncated CMPR rejected");
    check(!smg::decodeGxImage({cmpr.data(), 32}, GxFormat::CMPR, 2048, 8, {}, smg::GxPalette::RGB565, rejected, error),
          "unsupported texture dimension rejected");

    Data i4(32, 0);
    i4[0] = 0xf3;
    texture = image(GxFormat::I4, 8, 8, i4);
    check(texture.pixels[0] == 0xffffffff && texture.pixels[1] == 0x33333333 && texture.grayscale, "I4 nibble order, alpha is intensity");
    Data i8(32, 0);
    i8[9] = 0x80;
    texture = image(GxFormat::I8, 8, 4, i8);
    check(texture.pixels[9] == 0x80808080, "I8 block rows of eight");
    Data ia4(32, 0);
    ia4[0] = 0x5a;
    check(image(GxFormat::IA4, 8, 4, ia4).pixels[0] == 0xaaaaaa55, "IA4 alpha in the high nibble");
    Data ia8(32, 0);
    put16(ia8, 2, 0x40c0);
    check(image(GxFormat::IA8, 4, 4, ia8).pixels[1] == 0xc0c0c040, "IA8 alpha byte first");
    Data rgb5a3(32, 0);
    put16(rgb5a3, 0, 0xfc00);
    put16(rgb5a3, 2, 0x3f00);
    texture = image(GxFormat::RGB5A3, 4, 4, rgb5a3);
    check(texture.pixels[0] == 0xff0000ff && texture.pixels[1] == 0xff00006d && !texture.grayscale, "RGB5A3 opaque and 3-bit alpha");
    Data rgba8(64, 0);
    rgba8[0] = 0x11; rgba8[1] = 0x22; rgba8[32] = 0x33; rgba8[33] = 0x44;
    check(image(GxFormat::RGBA8, 4, 4, rgba8).pixels[0] == 0x22334411, "RGBA8 AR then GB halves");
    Data c4(32, 0), palette(32, 0);
    c4[0] = 0x1f;
    put16(palette, 2, 0x07e0);
    put16(palette, 30, 0x001f);
    texture = image(GxFormat::C4, 8, 8, c4, palette);
    check(texture.pixels[0] == 0x00ff00ff && texture.pixels[1] == 0x0000ffff, "C4 through an RGB565 palette");
    Data c8(32, 0), iaPalette(4, 0);
    c8[0] = 1;
    put16(iaPalette, 2, 0x8010);
    check(image(GxFormat::C8, 8, 4, c8, iaPalette, smg::GxPalette::IA8).pixels[0] == 0x10101080, "C8 through an IA8 palette");
    c8[1] = 7;
    check(!smg::decodeGxImage({c8.data(), c8.size()}, GxFormat::C8, 8, 4, {iaPalette.data(), iaPalette.size()},
                              smg::GxPalette::IA8, rejected, error), "palette index past the palette rejected");
    Data c14(32, 0);
    put16(c14, 0, 0xc001);
    check(image(GxFormat::C14X2, 4, 4, c14, iaPalette, smg::GxPalette::IA8).pixels[0] == 0x10101080, "C14X2 ignores the top two bits");
    check(image(GxFormat::I4, 3, 5, i4).pixels.size() == 15, "partial blocks are cropped");
}
} // namespace

int runModelTests() {
    models();
    textures();
    std::printf("%u J3D model/texture checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runModelTests(); }
#endif
