#include <smg/Model.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace smg {
namespace {
struct Reader {
    Bytes b;
    bool& valid;
    bool has(std::size_t at, std::size_t count) const { return b.data && b.contains(at, count); }
    unsigned u8(std::size_t at) const {
        if (!has(at, 1)) { valid = false; return 0; }
        return b.data[at];
    }
    unsigned u16(std::size_t at) const { return (u8(at) << 8) | u8(at + 1); }
    std::uint32_t u32(std::size_t at) const { return (std::uint32_t(u16(at)) << 16) | u16(at + 2); }
    float f32(std::size_t at) const {
        const auto bits = u32(at);
        float value;
        std::memcpy(&value, &bits, 4);
        if (!std::isfinite(value)) valid = false;
        return value;
    }
    bool table(std::size_t at, std::size_t count, std::size_t stride) const {
        return at <= b.size && stride && count <= (b.size - at) / stride;
    }
};

bool fail(std::string& error, const char* text) { error = text; return false; }

std::uint32_t rgb565(unsigned value) {
    const unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
    return (((r << 3) | (r >> 2)) << 24) | (((g << 2) | (g >> 4)) << 16) | (((b << 3) | (b >> 2)) << 8) | 255;
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, unsigned weightA, unsigned weightB) {
    std::uint32_t out = 255;
    for (int shift : {8, 16, 24})
        out |= (((a >> shift & 255) * weightA + (b >> shift & 255) * weightB) / (weightA + weightB)) << shift;
    return out;
}

// GX vertex attributes as numbered by VTX1/SHP1.
enum Attribute : unsigned { Position = 9, Normal = 10, Color0 = 11, Color1 = 12, Tex0 = 13 };

struct Format {
    unsigned components = 0, type = 0, shift = 0;
    std::size_t offset = 0, end = 0, stride = 0;
};

float component(const Reader& r, std::size_t at, unsigned type, unsigned shift) {
    float value = 0;
    if (type == 0) value = r.u8(at);
    else if (type == 1) value = static_cast<std::int8_t>(r.u8(at));
    else if (type == 2) value = r.u16(at);
    else if (type == 3) value = static_cast<std::int16_t>(r.u16(at));
    else return r.f32(at);
    return std::ldexp(value, -static_cast<int>(shift));
}

std::uint32_t color(const Reader& r, std::size_t at, unsigned type) {
    std::uint32_t c = 0;
    switch (type) {
    case 0: return rgb565(r.u16(at));
    case 1: return (r.u8(at) << 24) | (r.u8(at + 1) << 16) | (r.u8(at + 2) << 8) | 255;
    case 2: return r.u32(at) | 255;
    case 3: {
        const unsigned packed = r.u16(at);
        for (int i = 0; i < 4; ++i) c |= ((packed >> (12 - i * 4) & 15) * 17) << (24 - i * 8);
        return c;
    }
    case 4: {
        const unsigned packed = (r.u8(at) << 16) | (r.u8(at + 1) << 8) | r.u8(at + 2);
        for (int i = 0; i < 4; ++i) {
            const unsigned p = packed >> (18 - i * 6) & 63;
            c |= ((p << 2) | (p >> 4)) << (24 - i * 8);
        }
        return c;
    }
    default: return r.u32(at);
    }
}

bool attribute(const Reader& r, const Format& fmt, unsigned attr, unsigned index, ModelVertex& v) {
    if (!fmt.stride || fmt.end < fmt.offset || index >= (fmt.end - fmt.offset) / fmt.stride) return false;
    const auto at = fmt.offset + index * fmt.stride;
    if (attr == Color0 || attr == Color1) {
        if (fmt.type > 5) return false;
        const auto c = color(r, at, fmt.type);
        if (attr == Color0) for (int i = 0; i < 4; ++i) v.color[i] = (c >> (24 - i * 8) & 255) / 255.0f;
    } else {
        float* dst = attr == Position ? v.position : attr == Normal ? v.normal : v.uv;
        const unsigned step = fmt.type < 2 ? 1 : fmt.type < 4 ? 2 : 4;
        for (unsigned i = 0; i < fmt.components; ++i) dst[i] = component(r, at + i * step, fmt.type, fmt.shift);
    }
    return r.valid;
}

struct Sections {
    Reader inf, vtx, shp, jnt, drw, mat, tex;
};

bool checkRootJoint(const Sections& s, std::string& error) {
    const auto& jnt = s.jnt;
    const auto& drw = s.drw;
    if (jnt.u16(8) != 1 || drw.u16(8) != 1 || drw.u8(drw.u32(12)) != 0 || drw.u16(drw.u32(16)) != 0)
        return fail(error, "Skeletal models need the pending J3D matrix palette implementation");
    const auto joint = jnt.u32(12);
    if (!jnt.has(joint, 64) || jnt.u16(jnt.u32(16)) != 0) return fail(error, "Invalid root joint");
    for (unsigned i = 0; i < 3; ++i)
        if (jnt.f32(joint + 4 + i * 4) != 1 || jnt.u16(joint + 16 + i * 2) != 0 || jnt.f32(joint + 24 + i * 4) != 0)
            return fail(error, "Nonidentity root joint requires matrix transformation");
    return true;
}

bool readTextures(const Reader& tex, Model& model, std::size_t& remaining, std::string& error) {
    for (unsigned i = 0; i < model.textures.size(); ++i) {
        const std::size_t at = tex.u32(12) + i * 32;
        auto& texture = model.textures[i];
        texture.width = tex.u16(at + 2);
        texture.height = tex.u16(at + 4);
        texture.wrapS = tex.u8(at + 6);
        texture.wrapT = tex.u8(at + 7);
        if (tex.u8(at) != 14) return fail(error, "Texture format needs a native decoder (currently CMPR)");
        const auto pixels = std::size_t(texture.width) * texture.height;
        if (pixels > remaining / 4) return fail(error, "Model textures exceed decoded memory budget");
        const std::size_t relative = tex.u32(at + 28);
        if (relative > tex.b.size - at) return fail(error, "J3D texture data outside section");
        const std::size_t data = at + relative;
        if (!decodeCmpr({tex.b.data + data, tex.b.size - data}, texture.width, texture.height, texture, error))
            return false;
        remaining -= pixels * 4;
    }
    return true;
}

bool readMaterials(const Reader& mat, Model& model, std::string& error) {
    for (unsigned i = 0; i < model.materials.size(); ++i) {
        const std::size_t at = mat.u32(12) + mat.u16(mat.u32(16) + i * 2) * 332;
        if (!mat.has(at, 332)) return fail(error, "Invalid J3D material record");
        auto& m = model.materials[i];
        m.cullMode = mat.u32(mat.u32(28) + mat.u8(at + 1) * 4);
        m.tevStages = mat.u8(mat.u32(88) + mat.u8(at + 4));
        const unsigned colorIndex = mat.u16(at + 8);
        if (colorIndex != 65535)
            for (unsigned c = 0; c < 4; ++c) m.color[c] = mat.u8(mat.u32(32) + colorIndex * 4 + c) / 255.0f;
        const unsigned textureIndex = mat.u16(at + 132);
        if (textureIndex != 65535) {
            m.texture = mat.u16(mat.u32(72) + textureIndex * 2);
            if (m.texture >= static_cast<int>(model.textures.size()))
                return fail(error, "Invalid J3D material texture index");
        }
        const unsigned transformIndex = mat.u16(at + 72);
        if (transformIndex == 65535) continue;
        const std::size_t transform = mat.u32(64) + transformIndex * 100;
        if (!mat.has(transform, 100)) return fail(error, "Invalid texture matrix");
        if (mat.u8(transform + 1) != 0) continue;
        const float cx = mat.f32(transform + 4), cy = mat.f32(transform + 8);
        const float sx = mat.f32(transform + 16), sy = mat.f32(transform + 20);
        const float angle = static_cast<std::int16_t>(mat.u16(transform + 24)) * 3.14159265359f / 32768;
        const float s = std::sin(angle), c = std::cos(angle);
        m.uvTransform = {sx * c, -sx * s, -sx * c * cx + sx * s * cy + cx + mat.f32(transform + 28),
                         sy * s, sy * c, -sy * s * cx - sy * c * cy + cy + mat.f32(transform + 32)};
    }
    return true;
}

bool readHierarchy(const Reader& inf, Model& model, std::string& error) {
    const unsigned materialCount = model.materials.size(), shapeCount = model.shapes.size();
    unsigned currentMaterial = 0;
    bool ended = false;
    std::vector<unsigned> materialStack;
    std::vector<bool> assigned(shapeCount, false);
    if (inf.u32(20) < 24) return fail(error, "Invalid hierarchy offset");
    for (std::size_t at = inf.u32(20); inf.has(at, 4); at += 4) {
        const auto op = inf.u16(at), index = inf.u16(at + 2);
        if (!op) { ended = materialStack.empty(); break; }
        if (op == 0x11) {
            if (index >= materialCount) return fail(error, "Invalid hierarchy material");
            currentMaterial = index;
        } else if (op == 0x12) {
            if (index >= shapeCount || assigned[index]) return fail(error, "Invalid hierarchy shape");
            model.shapes[index].material = currentMaterial;
            assigned[index] = true;
        } else if (op == 1) {
            materialStack.push_back(currentMaterial);
        } else if (op == 2) {
            if (materialStack.empty()) return fail(error, "Unbalanced J3D hierarchy");
            currentMaterial = materialStack.back();
            materialStack.pop_back();
        } else if (op != 0x10 || index != 0) {
            return fail(error, "Unsupported J3D hierarchy command");
        }
    }
    if (!ended || std::find(assigned.begin(), assigned.end(), false) != assigned.end())
        return fail(error, "Incomplete J3D hierarchy");
    return true;
}

bool readVertexFormats(const Reader& vtx, std::array<Format, 21>& formats, std::string& error) {
    // Each array ends where the next one begins; VTX1 does not store array lengths.
    std::vector<std::size_t> offsets{vtx.b.size};
    for (unsigned i = 0; i < 14; ++i) {
        const auto at = vtx.u32(8 + i * 4);
        if (at) offsets.push_back(at);
    }
    for (std::size_t at = vtx.u32(8); vtx.has(at, 16); at += 16) {
        const auto attr = vtx.u32(at);
        if (attr == 255) {
            if (!formats[Position].stride) return fail(error, "Missing vertex position format");
            return true;
        }
        if (attr < Position || attr > Tex0 || formats[attr].stride)
            return fail(error, "Unsupported or duplicate vertex format");
        auto& f = formats[attr];
        const auto components = vtx.u32(at + 4);
        f.type = vtx.u32(at + 8);
        f.shift = vtx.u8(at + 12);
        f.offset = vtx.u32(attr == Position ? 12 : attr == Normal ? 16 : attr == Color0 ? 24 : attr == Color1 ? 28 : 32);
        f.end = vtx.b.size;
        for (auto offset : offsets) if (offset > f.offset) f.end = std::min(f.end, offset);
        if (!f.offset || f.offset >= f.end || f.end > vtx.b.size || f.shift > 31)
            return fail(error, "Invalid vertex array bounds");
        if (attr == Color0 || attr == Color1) {
            const unsigned widths[]{2, 3, 4, 2, 3, 4};
            if (f.type > 5) return fail(error, "Invalid vertex color format");
            f.stride = widths[f.type];
        } else {
            if (f.type > 4 || components > 1 || (attr == Normal && components != 0))
                return fail(error, "Unsupported vertex component format");
            f.components = attr == Position ? 2 + components : attr == Normal ? 3 : 1 + components;
            f.stride = f.components * (f.type < 2 ? 1 : f.type < 4 ? 2 : 4);
        }
    }
    return fail(error, "Missing vertex position format");
}

bool readShapes(const Sections& s, const std::array<Format, 21>& formats, Model& model,
                std::size_t& remaining, std::string& error) {
    const auto& shp = s.shp;
    for (unsigned i = 0; i < model.shapes.size(); ++i) {
        auto& shape = model.shapes[i];
        shape.first = model.vertices.size();
        const std::size_t init = shp.u32(12) + shp.u16(shp.u32(16) + i * 2) * 40;
        if (!shp.has(init, 40) || shp.u8(init) != 0) return fail(error, "Unsupported shape matrix mode");
        std::vector<std::pair<unsigned, unsigned>> descriptors;
        unsigned vertexSize = 0;
        bool descEnd = false, hasPosition = false;
        for (std::size_t at = shp.u32(24) + shp.u16(init + 4); shp.has(at, 8); at += 8) {
            const auto attr = shp.u32(at), type = shp.u32(at + 4);
            if (attr == 255) { descEnd = true; break; }
            if (attr < Position || attr > Tex0 || (type != 2 && type != 3) || !formats[attr].stride)
                return fail(error, "Unsupported shape vertex descriptor");
            if (std::any_of(descriptors.begin(), descriptors.end(), [&](const auto& d) { return d.first == attr; }))
                return fail(error, "Duplicate shape vertex descriptor");
            hasPosition |= attr == Position;
            descriptors.push_back({attr, type});
            vertexSize += type == 2 ? 1 : 2;
        }
        if (!descEnd || !hasPosition) return fail(error, "Missing shape position descriptor");
        const auto groups = shp.u16(init + 2);
        for (unsigned group = 0; group < groups; ++group) {
            const std::size_t matrixInfo = shp.u32(36) + (shp.u16(init + 6) + group) * 8;
            const std::size_t drawInfo = shp.u32(40) + (shp.u16(init + 8) + group) * 8;
            if (!shp.has(matrixInfo, 8) || !shp.has(drawInfo, 8) || shp.u16(matrixInfo) != 0)
                return fail(error, "Unsupported shape matrix group");
            const std::size_t size = shp.u32(drawInfo), base = shp.u32(32), relative = shp.u32(drawInfo + 4);
            if (relative > shp.b.size - base) return fail(error, "Shape display list offset outside block");
            const std::size_t begin = base + relative;
            if (!shp.has(begin, size)) return fail(error, "Shape display list outside block");
            for (std::size_t at = begin; at < begin + size;) {
                const unsigned command = shp.u8(at++);
                if (!command) continue;
                if ((command & 7) || (command != 0x90 && command != 0x98 && command != 0xa0 && command != 0x80))
                    return fail(error, "Unsupported GX primitive command");
                if (begin + size - at < 2) return fail(error, "Truncated GX primitive count");
                const unsigned count = shp.u16(at);
                at += 2;
                if (count > (begin + size - at) / vertexSize || count < 3 ||
                    (command == 0x90 && count % 3) || (command == 0x80 && count % 4))
                    return fail(error, "Invalid GX primitive vertex count");
                const std::size_t triangleVertices = command == 0x90 ? count : command == 0x80 ? count / 4 * 6 : (count - 2) * 3;
                if (triangleVertices > remaining / sizeof(ModelVertex))
                    return fail(error, "Model geometry exceeds decoded memory budget");
                std::vector<ModelVertex> vertices(count);
                for (auto& vertex : vertices) {
                    for (const auto& d : descriptors) {
                        const unsigned index = d.second == 2 ? shp.u8(at) : shp.u16(at);
                        at += d.second == 2 ? 1 : 2;
                        if (!attribute(s.vtx, formats[d.first], d.first, index, vertex))
                            return fail(error, "Invalid indexed vertex attribute");
                    }
                }
                const auto triangle = [&](unsigned a, unsigned b, unsigned c) {
                    model.vertices.push_back(vertices[a]);
                    model.vertices.push_back(vertices[b]);
                    model.vertices.push_back(vertices[c]);
                };
                if (command == 0x90) {
                    for (unsigned v = 0; v < count; v += 3) triangle(v, v + 1, v + 2);
                } else if (command == 0x80) {
                    for (unsigned v = 0; v < count; v += 4) {
                        triangle(v, v + 1, v + 2);
                        triangle(v, v + 2, v + 3);
                    }
                } else {
                    for (unsigned v = 2; v < count; ++v) {
                        if (command == 0xa0) triangle(0, v - 1, v);
                        else if (v & 1) triangle(v - 1, v - 2, v);
                        else triangle(v - 2, v - 1, v);
                    }
                }
                remaining -= triangleVertices * sizeof(ModelVertex);
            }
        }
        shape.count = model.vertices.size() - shape.first;
    }
    return true;
}
} // namespace

bool decodeCmpr(Bytes source, unsigned width, unsigned height, ModelTexture& output, std::string& error) {
    output.pixels.clear();
    error.clear();
    if (!width || !height || width > 1024 || height > 1024) return fail(error, "Unsupported CMPR dimensions");
    const std::size_t blocksX = (width + 7) / 8, blocksY = (height + 7) / 8;
    if (!source.data || !source.contains(0, blocksX * blocksY * 32)) return fail(error, "Truncated CMPR image");
    output.width = width;
    output.height = height;
    output.pixels.resize(width * height);
    bool valid = true;
    Reader r{source, valid};
    for (std::size_t by = 0; by < blocksY; ++by) for (std::size_t bx = 0; bx < blocksX; ++bx) {
        for (unsigned sub = 0; sub < 4; ++sub) {
            const auto at = ((by * blocksX + bx) * 4 + sub) * 8;
            const unsigned a = r.u16(at), b = r.u16(at + 2);
            std::uint32_t colors[4]{rgb565(a), rgb565(b), 0, 0};
            colors[2] = a > b ? mix(colors[0], colors[1], 2, 1) : mix(colors[0], colors[1], 1, 1);
            colors[3] = a > b ? mix(colors[0], colors[1], 1, 2) : (colors[2] & 0xffffff00);
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
                const unsigned px = bx * 8 + (sub % 2) * 4 + x, py = by * 8 + (sub / 2) * 4 + y;
                if (px < width && py < height)
                    output.pixels[py * width + px] = colors[r.u8(at + 4 + y) >> (6 - 2 * x) & 3];
            }
        }
    }
    return true;
}

bool loadModel(Bytes source, Model& output, std::size_t decodedBudget, std::string& error) {
    output = {};
    error.clear();
    bool valid = true;
    Reader file{source, valid};
    if (!file.has(0, 32) || std::memcmp(source.data, "J3D2", 4) ||
        (std::memcmp(source.data + 4, "bmd3", 4) && std::memcmp(source.data + 4, "bdl4", 4)))
        return fail(error, "Not a supported J3D2 BMD/BDL model");
    const auto fileSize = file.u32(8), blockCount = file.u32(12);
    if (fileSize > source.size || fileSize < 32 || blockCount > (fileSize - 32) / 8)
        return fail(error, "Invalid J3D file bounds");
    std::map<std::uint32_t, Bytes> blocks;
    for (std::size_t at = 32, i = 0; i < blockCount; ++i) {
        if (at > fileSize || fileSize - at < 8) return fail(error, "Truncated J3D block header");
        const auto tag = file.u32(at), size = file.u32(at + 4);
        if (size < 8 || size > fileSize - at || !blocks.emplace(tag, Bytes{source.data + at, size}).second)
            return fail(error, "Invalid or duplicate J3D block");
        at += size;
    }
    const auto section = [&](const char* tag) {
        const auto key = (std::uint32_t(tag[0]) << 24) | (tag[1] << 16) | (tag[2] << 8) | tag[3];
        return Reader{blocks[key], valid};
    };
    const Sections s{section("INF1"), section("VTX1"), section("SHP1"), section("JNT1"),
                     section("DRW1"), section("MAT3"), section("TEX1")};
    if (!s.inf.has(0, 24) || !s.vtx.has(0, 64) || !s.shp.has(0, 44) || !s.jnt.has(0, 24) ||
        !s.drw.has(0, 20) || !s.mat.has(0, 132) || !s.tex.has(0, 20))
        return fail(error, "Missing required J3D model sections");
    const std::pair<const Reader*, unsigned> headers[]{
        {&s.mat, 132}, {&s.vtx, 64}, {&s.shp, 44}, {&s.jnt, 24}, {&s.drw, 20}, {&s.tex, 20}};
    for (const auto& [reader, end] : headers) {
        for (unsigned at = reader == &s.vtx ? 8 : 12; at < end; at += 4)
            if (reader->u32(at) > reader->b.size) return fail(error, "J3D section offset exceeds bounds");
    }
    if (!checkRootJoint(s, error)) return false;

    Model model;
    std::size_t remaining = decodedBudget;
    const auto materialCount = s.mat.u16(8), shapeCount = s.shp.u16(8), textureCount = s.tex.u16(8);
    if (!materialCount || !shapeCount || !s.mat.table(s.mat.u32(16), materialCount, 2) ||
        !s.shp.table(s.shp.u32(16), shapeCount, 2) || !s.tex.table(s.tex.u32(12), textureCount, 32))
        return fail(error, "Invalid J3D object tables");
    model.materials.resize(materialCount);
    model.shapes.resize(shapeCount);
    model.textures.resize(textureCount);
    std::array<Format, 21> formats{};
    if (!readTextures(s.tex, model, remaining, error) || !readMaterials(s.mat, model, error) ||
        !readHierarchy(s.inf, model, error) || !readVertexFormats(s.vtx, formats, error) ||
        !readShapes(s, formats, model, remaining, error))
        return false;
    if (!valid || model.vertices.empty()) return fail(error, "Invalid or empty J3D model");
    for (unsigned i = 0; i < 3; ++i) model.minimum[i] = model.maximum[i] = model.vertices[0].position[i];
    for (const auto& v : model.vertices) {
        for (unsigned i = 0; i < 3; ++i) {
            model.minimum[i] = std::min(model.minimum[i], v.position[i]);
            model.maximum[i] = std::max(model.maximum[i], v.position[i]);
        }
    }
    output = std::move(model);
    return true;
}
} // namespace smg
