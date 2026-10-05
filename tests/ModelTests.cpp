#include "BmdFixture.hpp"
#include <smg/Mesh.hpp>
#include <smg/Model.hpp>
#include <smg/Texture.hpp>
#include <cmath>
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
bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

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
    check(!model.skinned, "rigid joints are not skinning");
    // A quarter turn about Y sends local +X to -Z before the child's translation.
    const auto& moved = model.vertices[0].position;
    check(near(moved[0], 10) && near(moved[1], -1) && near(moved[2], 1), "rigid vertices use their joint's bind matrix");
    Options skinned;
    skinned.envelope = true;
    check(load(fixture(skinned).data, model, 4096, error), "enveloped model decoded");
    check(near(model.vertices[0].position[0], 4.5f) && near(model.vertices[0].position[2], 0.5f), "envelope weights blend joint matrices");
    check(model.skinned, "enveloped models are marked skinned");

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

void meshes() {
    smg::Model model;
    std::string error;
    check(load(fixture().data, model, 4096, error), "quad for packing");
    auto mesh = smg::packMesh(model);
    check(mesh.vertices.size() == 4 && mesh.indices.size() == 6 && mesh.shapes.size() == 1, "strip corners are shared");
    check(mesh.vertices[mesh.indices[0]].position[0] == -1 && mesh.indices[3] == mesh.indices[2] && mesh.indices[4] == mesh.indices[1], "indices keep triangle order");
    check(near(mesh.radius, std::sqrt(2.0f)), "bounding sphere");
    check(mesh.vertices[0].color[3] == 255 && mesh.vertices[0].normal[1] == 127, "colors and normals as bytes");

    // Enough distinct triangles to overflow 16-bit indices must split into another draw.
    smg::Model big;
    big.shapes.push_back({0, 0, 0, 0});
    for (unsigned i = 0; i < 70000; ++i) {
        smg::ModelVertex v;
        v.position[0] = static_cast<float>(i);
        big.vertices.push_back(v);
    }
    big.vertices.resize(69999);
    big.shapes[0].count = big.vertices.size();
    mesh = smg::packMesh(big);
    bool fits = mesh.shapes.size() == 2;
    for (const auto& shape : mesh.shapes) {
        const std::size_t next = &shape == &mesh.shapes.back() ? mesh.vertices.size() : (&shape)[1].firstVertex;
        fits &= next - shape.firstVertex <= 65536 && shape.indexCount % 3 == 0;
    }
    check(fits && mesh.indices.size() == 69999, "large shapes split at whole triangles");
}

// The renderer's direct path for I8 (SnowFloor's footprints, drawn anew each frame): every texel lands where
// decoding and the PICA tiler put it, rows from the bottom in 8x8 Morton tiles, both LA8 bytes its intensity.
void intensityTiles() {
    const unsigned width = 16, height = 8;
    Data i8(width * height);
    for (std::size_t i = 0; i < i8.size(); ++i) i8[i] = static_cast<std::uint8_t>(i * 37 + 11);
    const auto decoded = image(smg::GxFormat::I8, width, height, i8);
    std::vector<std::uint8_t> la8(width * height * 2);
    smg::tileIntensity8(i8.data(), width, height, la8.data());
    const auto morton = [](unsigned x, unsigned y) {
        return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    };
    bool same = true;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            const std::uint8_t want = static_cast<std::uint8_t>(decoded.pixels[(height - 1 - y) * width + x] >> 24);
            const unsigned texel = ((y / 8) * (width / 8) + x / 8) * 64 + morton(x, y);
            same &= la8[texel * 2] == want && la8[texel * 2 + 1] == want;
        }
    check(same, "I8 tiles straight into the PICA's LA8 layout");
}

int runModelTests() {
    models();
    textures();
    intensityTiles();
    meshes();
    std::printf("%u J3D model/texture checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runModelTests(); }
#endif
