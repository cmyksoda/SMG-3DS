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
    std::int16_t s16(std::size_t at) const { return static_cast<std::int16_t>(u16(at)); }
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

using Mtx34 = std::array<float, 12>;
constexpr Mtx34 Identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};

Mtx34 multiply(const Mtx34& a, const Mtx34& b) {
    Mtx34 r{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = col == 3 ? a[row * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) sum += a[row * 4 + k] * b[k * 4 + col];
            r[row * 4 + col] = sum;
        }
    }
    return r;
}

// J3D joints are T * Rz * Ry * Rx * S, with angles in 1/32768ths of a half turn.
Mtx34 jointMatrix(const float scale[3], const std::int16_t rotation[3], const float translation[3]) {
    constexpr float toRadians = 3.14159265358979f / 32768.0f;
    const float sx = std::sin(rotation[0] * toRadians), cx = std::cos(rotation[0] * toRadians);
    const float sy = std::sin(rotation[1] * toRadians), cy = std::cos(rotation[1] * toRadians);
    const float sz = std::sin(rotation[2] * toRadians), cz = std::cos(rotation[2] * toRadians);
    const Mtx34 r{
        cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, translation[0],
        sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, translation[1],
        -sy, cy * sx, cy * cx, translation[2]};
    Mtx34 m = r;
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) m[row * 4 + col] *= scale[col];
    return m;
}

void transform(const Mtx34& m, ModelVertex& v) {
    float p[3], n[3];
    for (int row = 0; row < 3; ++row) {
        p[row] = m[row * 4] * v.position[0] + m[row * 4 + 1] * v.position[1] + m[row * 4 + 2] * v.position[2] + m[row * 4 + 3];
        n[row] = m[row * 4] * v.normal[0] + m[row * 4 + 1] * v.normal[1] + m[row * 4 + 2] * v.normal[2];
    }
    const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (int i = 0; i < 3; ++i) {
        v.position[i] = p[i];
        v.normal[i] = length > 1e-6f ? n[i] / length : v.normal[i];
    }
}

// GX vertex attributes as numbered by VTX1/SHP1.
enum Attribute : unsigned { MatrixIndex = 0, Position = 9, Normal = 10, Color0 = 11, Color1 = 12, Tex0 = 13, Tex1 = 14, Tex7 = 20 };

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

std::uint32_t rgb565(unsigned value) {
    const unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
    return (((r << 3) | (r >> 2)) << 24) | (((g << 2) | (g >> 4)) << 16) | (((b << 3) | (b >> 2)) << 8) | 255;
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
        const auto c = color(r, at, fmt.type);
        if (attr == Color0) for (int i = 0; i < 4; ++i) v.color[i] = (c >> (24 - i * 8) & 255) / 255.0f;
    } else if (attr == Position || attr == Normal || attr == Tex0 || attr == Tex1) {
        float* dst = attr == Position ? v.position : attr == Normal ? v.normal : v.uv[attr - Tex0];
        const unsigned step = fmt.type < 2 ? 1 : fmt.type < 4 ? 2 : 4;
        const unsigned count = attr == Normal ? 3 : fmt.components;
        for (unsigned i = 0; i < count; ++i) dst[i] = component(r, at + i * step, fmt.type, fmt.shift);
    }
    return r.valid;
}

struct Sections {
    Reader inf, vtx, evp, drw, jnt, shp, mat, tex;
};

std::vector<std::string> names(const Reader& r, std::size_t table) {
    std::vector<std::string> result;
    if (!table || !r.has(table, 4)) return result;
    const unsigned count = r.u16(table);
    for (unsigned i = 0; i < count && r.has(table + 4 + i * 4, 4); ++i) {
        const std::size_t at = table + r.u16(table + 6 + i * 4);
        std::string name;
        for (std::size_t c = at; r.has(c, 1) && r.b.data[c] && name.size() < 256; ++c) name += static_cast<char>(r.b.data[c]);
        result.push_back(name);
    }
    return result;
}

bool readTextures(const Reader& tex, Model& model, std::size_t& remaining, std::string& error) {
    const auto textureNames = names(tex, tex.u32(16));
    for (unsigned i = 0; i < model.textures.size(); ++i) {
        const std::size_t at = tex.u32(12) + i * 32;
        auto& texture = model.textures[i];
        if (i < textureNames.size()) texture.name = textureNames[i];
        texture.format = static_cast<GxFormat>(tex.u8(at));
        const unsigned width = tex.u16(at + 2), height = tex.u16(at + 4);
        texture.wrapS = tex.u8(at + 6);
        texture.wrapT = tex.u8(at + 7);
        texture.minFilter = tex.u8(at + 20);
        texture.magFilter = tex.u8(at + 21);
        const auto paletteFormat = static_cast<GxPalette>(tex.u8(at + 9));
        const unsigned paletteCount = tex.u16(at + 10);
        const std::size_t paletteRelative = tex.u32(at + 12), dataRelative = tex.u32(at + 28);
        const auto pixels = std::size_t(width) * height;
        if (pixels > remaining / 4) return fail(error, "Model textures exceed decoded memory budget");
        if (dataRelative > tex.b.size - at || paletteRelative > tex.b.size - at)
            return fail(error, "J3D texture data outside section");
        const std::size_t data = at + dataRelative, palette = at + paletteRelative;
        const std::size_t paletteBytes = std::min<std::size_t>(paletteCount * 2u, tex.b.size - palette);
        std::string reason;
        if (!decodeGxImage({tex.b.data + data, tex.b.size - data}, texture.format, width, height,
                           {tex.b.data + palette, paletteBytes}, paletteFormat, texture.image, reason)) {
            error = "Texture " + texture.name + ": " + reason;
            return false;
        }
        remaining -= pixels * 4;
    }
    return true;
}

TexMatrix readTexMatrix(const Reader& mat, std::size_t at) {
    TexMatrix m;
    m.projection = mat.u8(at);
    m.mapping = mat.u8(at + 1);
    for (int i = 0; i < 3; ++i) m.center[i] = mat.f32(at + 4 + i * 4);
    m.scale[0] = mat.f32(at + 16);
    m.scale[1] = mat.f32(at + 20);
    m.rotation = mat.s16(at + 24) * 3.14159265358979f / 32768.0f;
    m.translation[0] = mat.f32(at + 28);
    m.translation[1] = mat.f32(at + 32);
    const float s = std::sin(m.rotation), c = std::cos(m.rotation);
    const float ss = m.scale[0], st = m.scale[1], ts = m.translation[0], tt = m.translation[1];
    if (m.mapping & 0x80) {
        // Maya's convention pivots about the texture's center and flips T.
        m.uv = {ss * c, -st * s, ss * (-0.5f * c - (0.5f * s - 0.5f) - ts),
                ss * s, st * c, st * (-0.5f * c + (0.5f * s - 0.5f) + tt) + 1.0f};
    } else {
        const float cs = m.center[0], ct = m.center[1];
        m.uv = {ss * c, -ss * s, ts + cs - (ss * c * cs - ss * s * ct),
                st * s, st * c, tt + ct - (st * s * cs + st * c * ct)};
    }
    return m;
}

bool readMaterials(const Reader& mat, Model& model, std::string& error) {
    const auto table = [&](unsigned header) { return static_cast<std::size_t>(mat.u32(header)); };
    // An absent table (offset zero) or an unused slot (0xffff) keeps the GX default.
    const auto entry = [&](unsigned header, unsigned index, std::size_t stride, std::size_t& at) {
        if (index == 0xffff || !table(header)) return false;
        at = table(header) + index * stride;
        return mat.has(at, stride);
    };
    const auto materialNames = names(mat, table(0x14));
    std::size_t at = 0;
    for (unsigned i = 0; i < model.materials.size(); ++i) {
        const std::size_t base = table(0x0c) + mat.u16(table(0x10) + i * 2) * 0x14c;
        if (!mat.has(base, 0x14c)) return fail(error, "Invalid J3D material record");
        auto& m = model.materials[i];
        if (i < materialNames.size()) m.name = materialNames[i];
        m.mode = mat.u8(base);
        if (entry(0x1c, mat.u8(base + 1), 4, at)) m.cullMode = mat.u32(at);
        if (entry(0x24, mat.u8(base + 2), 1, at)) m.channelCount = mat.u8(at);
        if (entry(0x34, mat.u8(base + 3), 1, at)) m.texGenCount = std::min(8u, mat.u8(at));
        if (entry(0x58, mat.u8(base + 4), 1, at)) m.tevStageCount = std::clamp(mat.u8(at), 1u, 16u);
        if (entry(0x74, mat.u8(base + 6), 4, at)) m.depth = {mat.u8(at) != 0, static_cast<std::uint8_t>(mat.u8(at + 1)), mat.u8(at + 2) != 0};
        for (unsigned k = 0; k < 2; ++k) {
            if (entry(0x20, mat.u16(base + 0x08 + k * 2), 4, at))
                for (unsigned c = 0; c < 4; ++c) m.materialColor[k][c] = mat.u8(at + c) / 255.0f;
            if (entry(0x2c, mat.u16(base + 0x14 + k * 2), 4, at))
                for (unsigned c = 0; c < 4; ++c) m.ambientColor[k][c] = mat.u8(at + c) / 255.0f;
        }
        for (unsigned k = 0; k < 4; ++k) {
            if (!entry(0x28, mat.u16(base + 0x0c + k * 2), 8, at)) continue;
            auto& channel = m.channels[k];
            channel.lighting = mat.u8(at) != 0;
            channel.materialSource = mat.u8(at + 1);
            channel.lightMask = mat.u8(at + 2);
            channel.diffuse = mat.u8(at + 3);
            channel.attenuation = mat.u8(at + 4);
            channel.ambientSource = mat.u8(at + 5);
        }
        for (unsigned k = 0; k < 8; ++k) {
            if (entry(0x38, mat.u16(base + 0x28 + k * 2), 4, at))
                m.texGens[k] = {static_cast<std::uint8_t>(mat.u8(at)), static_cast<std::uint8_t>(mat.u8(at + 1)),
                                static_cast<std::uint8_t>(mat.u8(at + 2))};
            if (entry(0x48, mat.u16(base + 0x84 + k * 2), 2, at)) {
                m.textures[k] = mat.u16(at);
                if (m.textures[k] >= static_cast<int>(model.textures.size()))
                    return fail(error, "Invalid J3D material texture index");
            }
        }
        for (unsigned k = 0; k < 10; ++k) {
            if (!entry(0x40, mat.u16(base + 0x48 + k * 2), 100, at)) continue;
            m.texMatrices[k] = readTexMatrix(mat, at);
            m.hasTexMatrix[k] = true;
        }
        for (unsigned k = 0; k < 4; ++k) {
            if (entry(0x54, mat.u16(base + 0x94 + k * 2), 4, at))
                for (unsigned c = 0; c < 4; ++c) m.konstColors[k][c] = mat.u8(at + c) / 255.0f;
            if (entry(0x50, mat.u16(base + 0xdc + k * 2), 8, at))
                for (unsigned c = 0; c < 4; ++c) m.tevColors[k][c] = mat.s16(at + c * 2) / 255.0f;
            if (entry(0x64, mat.u16(base + 0x124 + k * 2), 4, at))
                for (unsigned c = 0; c < 4; ++c) m.swapTables[k][c] = mat.u8(at + c) & 3;
        }
        for (unsigned k = 0; k < 16; ++k) {
            auto& stage = m.stages[k];
            stage.konstColor = mat.u8(base + 0x9c + k);
            stage.konstAlpha = mat.u8(base + 0xac + k);
            if (entry(0x4c, mat.u16(base + 0xbc + k * 2), 4, at)) {
                stage.texCoord = mat.u8(at);
                stage.texMap = mat.u8(at + 1);
                stage.channel = mat.u8(at + 2);
            }
            if (entry(0x5c, mat.u16(base + 0xe4 + k * 2), 20, at)) {
                for (unsigned c = 0; c < 4; ++c) {
                    stage.colorIn[c] = mat.u8(at + 1 + c);
                    stage.alphaIn[c] = mat.u8(at + 10 + c);
                }
                stage.colorOp = mat.u8(at + 5);
                stage.colorBias = mat.u8(at + 6);
                stage.colorScale = mat.u8(at + 7);
                stage.colorClamp = mat.u8(at + 8);
                stage.colorReg = mat.u8(at + 9);
                stage.alphaOp = mat.u8(at + 14);
                stage.alphaBias = mat.u8(at + 15);
                stage.alphaScale = mat.u8(at + 16);
                stage.alphaClamp = mat.u8(at + 17);
                stage.alphaReg = mat.u8(at + 18);
            }
            if (entry(0x60, mat.u16(base + 0x104 + k * 2), 4, at)) {
                stage.rasSwap = mat.u8(at) & 3;
                stage.texSwap = mat.u8(at + 1) & 3;
            }
        }
        if (entry(0x6c, mat.u16(base + 0x146), 8, at))
            m.alphaTest = {static_cast<std::uint8_t>(mat.u8(at)), static_cast<std::uint8_t>(mat.u8(at + 1)),
                           static_cast<std::uint8_t>(mat.u8(at + 2)), static_cast<std::uint8_t>(mat.u8(at + 3)),
                           static_cast<std::uint8_t>(mat.u8(at + 4))};
        if (entry(0x70, mat.u16(base + 0x148), 4, at))
            m.blend = {static_cast<std::uint8_t>(mat.u8(at)), static_cast<std::uint8_t>(mat.u8(at + 1)),
                       static_cast<std::uint8_t>(mat.u8(at + 2)), static_cast<std::uint8_t>(mat.u8(at + 3))};
    }
    return true;
}


bool readHierarchy(const Reader& inf, unsigned jointCount, Model& model, std::string& error) {
    const unsigned materialCount = model.materials.size(), shapeCount = model.shapes.size();
    unsigned currentMaterial = 0;
    int lastJoint = -1;
    bool ended = false;
    std::vector<unsigned> materialStack;
    std::vector<int> jointStack;
    std::vector<bool> assigned(shapeCount, false), placed(jointCount, false);
    if (inf.u32(20) < 24) return fail(error, "Invalid hierarchy offset");
    for (std::size_t at = inf.u32(20); inf.has(at, 4); at += 4) {
        const auto op = inf.u16(at), index = inf.u16(at + 2);
        if (!op) { ended = materialStack.empty(); break; }
        if (op == 0x10) {
            if (index >= jointCount || placed[index]) return fail(error, "Invalid hierarchy joint");
            model.joints[index].parent = jointStack.empty() ? -1 : jointStack.back();
            placed[index] = true;
            lastJoint = index;
        } else if (op == 0x11) {
            if (index >= materialCount) return fail(error, "Invalid hierarchy material");
            currentMaterial = index;
        } else if (op == 0x12) {
            if (index >= shapeCount || assigned[index]) return fail(error, "Invalid hierarchy shape");
            model.shapes[index].material = currentMaterial;
            model.shapes[index].joint = jointStack.empty() ? -1 : jointStack.back();
            assigned[index] = true;
        } else if (op == 1) {
            materialStack.push_back(currentMaterial);
            jointStack.push_back(lastJoint);
        } else if (op == 2) {
            if (materialStack.empty()) return fail(error, "Unbalanced J3D hierarchy");
            currentMaterial = materialStack.back();
            materialStack.pop_back();
            jointStack.pop_back();
        } else {
            return fail(error, "Unsupported J3D hierarchy command");
        }
    }
    if (!ended || std::find(assigned.begin(), assigned.end(), false) != assigned.end() ||
        std::find(placed.begin(), placed.end(), false) != placed.end())
        return fail(error, "Incomplete J3D hierarchy");
    return true;
}

bool readJoints(const Sections& s, Model& model, std::string& error) {
    const auto& jnt = s.jnt;
    const unsigned count = jnt.u16(8);
    if (!count || !jnt.table(jnt.u32(16), count, 2)) return fail(error, "Invalid J3D joint table");
    model.joints.resize(count);
    const auto jointNames = names(jnt, jnt.u32(20));
    for (unsigned i = 0; i < count; ++i) if (i < jointNames.size()) model.joints[i].name = jointNames[i];
    if (!readHierarchy(s.inf, count, model, error)) return false;
    // Parents always precede children in INF1, so one pass in placement order resolves them.
    std::vector<Mtx34> world(count, Identity);
    std::vector<bool> done(count, false);
    for (unsigned pass = 0; pass < count; ++pass) {
        bool progressed = false;
        for (unsigned i = 0; i < count; ++i) {
            const int parent = model.joints[i].parent;
            if (done[i] || (parent >= 0 && !done[parent])) continue;
            const std::size_t at = jnt.u32(12) + jnt.u16(jnt.u32(16) + i * 2) * 0x40;
            if (!jnt.has(at, 0x40)) return fail(error, "Invalid J3D joint record");
            float scale[3], translation[3];
            std::int16_t rotation[3];
            for (int k = 0; k < 3; ++k) {
                scale[k] = jnt.f32(at + 4 + k * 4);
                rotation[k] = jnt.s16(at + 16 + k * 2);
                translation[k] = jnt.f32(at + 24 + k * 4);
            }
            auto& joint = model.joints[i];
            std::copy(scale, scale + 3, joint.scale);
            std::copy(translation, translation + 3, joint.translation);
            std::copy(rotation, rotation + 3, joint.rotation);
            const auto local = jointMatrix(scale, rotation, translation);
            world[i] = parent >= 0 ? multiply(world[parent], local) : local;
            std::copy(world[i].begin(), world[i].end(), model.joints[i].world);
            done[i] = progressed = true;
        }
        if (!progressed) break;
    }
    if (std::find(done.begin(), done.end(), false) != done.end()) return fail(error, "Cyclic J3D joint hierarchy");
    return true;
}

bool drawMatrices(const Sections& s, Model& model, std::vector<Mtx34>& result, bool& skinned, bool keepSkin, std::string& error) {
    const auto& drw = s.drw;
    const auto& evp = s.evp;
    const unsigned count = drw.u16(8);
    if (!count || !drw.table(drw.u32(12), count, 1) || !drw.table(drw.u32(16), count, 2))
        return fail(error, "Invalid J3D draw matrix table");
    const auto jointWorld = [&](unsigned joint) {
        Mtx34 m;
        std::copy(model.joints[joint].world, model.joints[joint].world + 12, m.begin());
        return m;
    };
    // Envelope indices and weights are packed back to back; find each envelope's start.
    std::vector<std::size_t> envelopeStart;
    const unsigned envelopes = evp.b.data ? evp.u16(8) : 0;
    std::size_t total = 0;
    for (unsigned e = 0; e < envelopes; ++e) {
        envelopeStart.push_back(total);
        total += evp.u8(evp.u32(12) + e);
    }
    result.assign(count, Identity);
    if (keepSkin) {
        model.skin.draws.assign(count, {});
        model.skin.inverseBind.assign(model.joints.size(), Identity);
    }
    for (unsigned i = 0; i < count; ++i) {
        const bool weighted = drw.u8(drw.u32(12) + i) != 0;
        const unsigned index = drw.u16(drw.u32(16) + i * 2);
        if (!weighted) {
            if (index >= model.joints.size()) return fail(error, "Invalid J3D draw matrix joint");
            result[i] = jointWorld(index);
            if (keepSkin) model.skin.draws[i].joints = {{static_cast<std::uint16_t>(index), 1.0f}};
            continue;
        }
        if (keepSkin) model.skin.draws[i].weighted = true;
        if (index >= envelopes) return fail(error, "Invalid J3D envelope index");
        skinned = true;
        Mtx34 sum{};
        const unsigned mix = evp.u8(evp.u32(12) + index);
        for (unsigned k = 0; k < mix; ++k) {
            const std::size_t slot = envelopeStart[index] + k;
            const unsigned joint = evp.u16(evp.u32(16) + slot * 2);
            const float weight = evp.f32(evp.u32(20) + slot * 4);
            if (joint >= model.joints.size()) return fail(error, "Invalid J3D envelope joint");
            Mtx34 inverse;
            for (int c = 0; c < 12; ++c) inverse[c] = evp.f32(evp.u32(24) + joint * 48 + c * 4);
            if (keepSkin) {
                model.skin.draws[i].joints.push_back({static_cast<std::uint16_t>(joint), weight});
                std::copy(inverse.begin(), inverse.end(), model.skin.inverseBind[joint].begin());
            }
            const auto skin = multiply(jointWorld(joint), inverse);
            for (int c = 0; c < 12; ++c) sum[c] += skin[c] * weight;
        }
        result[i] = sum;
    }
    if (!drw.valid || !evp.valid) return fail(error, "Truncated J3D envelope data");
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
        if (attr < Position || attr > Tex7 || formats[attr].stride)
            return fail(error, "Unsupported or duplicate vertex format");
        auto& f = formats[attr];
        const auto components = vtx.u32(at + 4);
        f.type = vtx.u32(at + 8);
        f.shift = vtx.u8(at + 12);
        const unsigned header = attr == Position ? 0x0c : attr == Normal ? 0x10 : attr == Color0 ? 0x18
                              : attr == Color1 ? 0x1c : 0x20 + (attr - Tex0) * 4;
        f.offset = vtx.u32(header);
        if (attr == Normal && !f.offset) f.offset = vtx.u32(0x14);
        f.end = vtx.b.size;
        for (auto offset : offsets) if (offset > f.offset) f.end = std::min(f.end, offset);
        if (!f.offset || f.offset >= f.end || f.end > vtx.b.size || f.shift > 31)
            return fail(error, "Invalid vertex array bounds");
        if (attr == Color0 || attr == Color1) {
            const unsigned widths[]{2, 3, 4, 2, 3, 4};
            if (f.type > 5) return fail(error, "Invalid vertex color format");
            f.stride = widths[f.type];
        } else {
            if (f.type > 4 || components > 1) return fail(error, "Unsupported vertex component format");
            // Normals may carry binormal and tangent (NBT) after them; only the normal is used.
            f.components = attr == Position ? 2 + components : attr == Normal ? (components ? 9 : 3) : 1 + components;
            f.stride = f.components * (f.type < 2 ? 1 : f.type < 4 ? 2 : 4);
        }
    }
    return fail(error, "Missing vertex position format");
}

bool readShapes(const Sections& s, const std::array<Format, 21>& formats, const std::vector<Mtx34>& draw,
                Model& model, std::size_t& remaining, bool keepSkin, std::string& error) {
    const auto& shp = s.shp;
    for (unsigned i = 0; i < model.shapes.size(); ++i) {
        auto& shape = model.shapes[i];
        shape.first = model.vertices.size();
        const std::size_t init = shp.u32(12) + shp.u16(shp.u32(16) + i * 2) * 40;
        if (!shp.has(init, 40) || shp.u8(init) > 3) return fail(error, "Unsupported shape matrix mode");
        shape.matrixType = shp.u8(init);
        struct Descriptor { unsigned attr, type; };
        std::vector<Descriptor> descriptors;
        unsigned vertexSize = 0;
        bool descEnd = false, hasPosition = false, hasMatrix = false;
        for (std::size_t at = shp.u32(24) + shp.u16(init + 4); shp.has(at, 8); at += 8) {
            const auto attr = shp.u32(at), type = shp.u32(at + 4);
            if (attr == 255) { descEnd = true; break; }
            const bool direct = attr < Position;
            if (attr > Tex7 || (direct ? type != 1 : (type != 2 && type != 3) || !formats[attr].stride))
                return fail(error, "Unsupported shape vertex descriptor");
            if (std::any_of(descriptors.begin(), descriptors.end(), [&](const Descriptor& d) { return d.attr == attr; }))
                return fail(error, "Duplicate shape vertex descriptor");
            hasPosition |= attr == Position;
            hasMatrix |= attr == MatrixIndex;
            descriptors.push_back({attr, type});
            vertexSize += type == 3 ? 2 : 1;
        }
        if (!descEnd || !hasPosition) return fail(error, "Missing shape position descriptor");
        const auto groups = shp.u16(init + 2);
        unsigned slots[10];
        std::fill(std::begin(slots), std::end(slots), 0xffffu);
        for (unsigned group = 0; group < groups; ++group) {
            const std::size_t matrixInfo = shp.u32(36) + (shp.u16(init + 6) + group) * 8;
            const std::size_t drawInfo = shp.u32(40) + (shp.u16(init + 8) + group) * 8;
            if (!shp.has(matrixInfo, 8) || !shp.has(drawInfo, 8)) return fail(error, "Invalid shape matrix group");
            const unsigned current = shp.u16(matrixInfo), slotCount = shp.u16(matrixInfo + 2);
            const std::size_t firstSlot = shp.u32(28) + std::size_t(shp.u32(matrixInfo + 4)) * 2;
            if (slotCount > 10 || !shp.has(firstSlot, slotCount * 2)) return fail(error, "Invalid shape matrix slots");
            // 0xffff keeps the matrix the previous packet left in that slot.
            for (unsigned k = 0; k < slotCount; ++k)
                if (const unsigned value = shp.u16(firstSlot + k * 2); value != 0xffff) slots[k] = value;
            if (!hasMatrix && current >= draw.size()) return fail(error, "Invalid shape draw matrix");
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
                std::vector<ModelVertex> vertices(count), rest(keepSkin ? count : 0);
                std::vector<std::uint16_t> matrices(keepSkin ? count : 0);
                for (std::size_t v = 0; v < count; ++v) {
                    auto& vertex = vertices[v];
                    unsigned matrix = current;
                    for (const auto& d : descriptors) {
                        const unsigned index = d.type == 3 ? shp.u16(at) : shp.u8(at);
                        at += d.type == 3 ? 2 : 1;
                        if (d.attr == MatrixIndex) {
                            if (index / 3 >= 10 || slots[index / 3] >= draw.size())
                                return fail(error, "Invalid vertex matrix index");
                            matrix = slots[index / 3];
                        } else if (d.attr >= Position && !attribute(s.vtx, formats[d.attr], d.attr, index, vertex)) {
                            return fail(error, "Invalid indexed vertex attribute");
                        }
                    }
                    if (keepSkin) rest[v] = vertex, matrices[v] = static_cast<std::uint16_t>(matrix);
                    transform(draw[matrix], vertex);
                }
                const auto triangle = [&](unsigned a, unsigned b, unsigned c) {
                    for (const unsigned k : {a, b, c}) {
                        model.vertices.push_back(vertices[k]);
                        if (!keepSkin) continue;
                        model.skin.rest.push_back(rest[k]);
                        model.skin.matrix.push_back(matrices[k]);
                    }
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

bool loadModel(Bytes source, Model& output, std::size_t decodedBudget, std::string& error, bool keepSkin) {
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
        const auto found = blocks.find(key);
        return Reader{found == blocks.end() ? Bytes{} : found->second, valid};
    };
    bool evpValid = true;
    Sections s{section("INF1"), section("VTX1"), Reader{Bytes{}, evpValid}, section("DRW1"),
               section("JNT1"), section("SHP1"), section("MAT3"), section("TEX1")};
    if (const auto found = blocks.find(0x45565031); found != blocks.end()) s.evp.b = found->second;
    if (!s.inf.has(0, 24) || !s.vtx.has(0, 64) || !s.shp.has(0, 44) || !s.jnt.has(0, 24) ||
        !s.drw.has(0, 20) || !s.mat.has(0, 132) || !s.tex.has(0, 20))
        return fail(error, "Missing required J3D model sections");
    if (s.evp.b.data && !s.evp.has(0, 28)) return fail(error, "Truncated J3D envelope section");
    const std::pair<const Reader*, unsigned> headers[]{
        {&s.mat, 132}, {&s.vtx, 64}, {&s.shp, 44}, {&s.jnt, 24}, {&s.drw, 20}, {&s.tex, 20}, {&s.evp, 28}};
    for (const auto& [reader, end] : headers) {
        if (!reader->b.data) continue;
        for (unsigned at = reader == &s.vtx ? 8 : 12; at < end; at += 4)
            if (reader->u32(at) > reader->b.size) return fail(error, "J3D section offset exceeds bounds");
    }

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
    std::vector<Mtx34> draw;
    if (!readTextures(s.tex, model, remaining, error) || !readMaterials(s.mat, model, error) ||
        !readJoints(s, model, error) || !drawMatrices(s, model, draw, model.skinned, keepSkin, error) ||
        !readVertexFormats(s.vtx, formats, error) || !readShapes(s, formats, draw, model, remaining, keepSkin, error))
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
void poseSkeleton(const Model& model, const std::vector<JointTransform>& local, std::vector<std::array<float, 12>>& world) {
    const std::size_t count = model.joints.size();
    world.assign(count, Identity);
    std::vector<bool> done(count, false);
    for (std::size_t pass = 0; pass < count; ++pass) {
        bool progressed = false;
        for (std::size_t i = 0; i < count; ++i) {
            const int parent = model.joints[i].parent;
            if (done[i] || (parent >= 0 && !done[parent])) continue;
            const JointTransform& t = i < local.size() ? local[i] : JointTransform{};
            const auto matrix = jointMatrix(t.scale, t.rotation, t.translation);
            world[i] = parent >= 0 ? multiply(world[parent], matrix) : matrix;
            done[i] = progressed = true;
        }
        if (!progressed) break;
    }
}

void skinMatrices(const Model& model, const std::vector<std::array<float, 12>>& world, std::vector<std::array<float, 12>>& draws) {
    const auto& skin = model.skin;
    draws.assign(skin.draws.size(), Identity);
    for (std::size_t d = 0; d < skin.draws.size(); ++d) {
        const auto& draw = skin.draws[d];
        if (!draw.weighted) {
            if (!draw.joints.empty()) draws[d] = world[draw.joints[0].first];
            continue;
        }
        Mtx34 sum{};
        for (const auto& [joint, weight] : draw.joints) {
            const auto m = multiply(world[joint], skin.inverseBind[joint]);
            for (int c = 0; c < 12; ++c) sum[c] += m[c] * weight;
        }
        draws[d] = sum;
    }
}

ModelVertex skinVertex(const Model& model, const std::vector<std::array<float, 12>>& draws, std::size_t vertex) {
    ModelVertex v = model.skin.rest[vertex];
    transform(draws[model.skin.matrix[vertex]], v);
    return v;
}

void skinModel(const Model& model, const std::vector<std::array<float, 12>>& world, std::vector<ModelVertex>& vertices) {
    std::vector<Mtx34> draws;
    skinMatrices(model, world, draws);
    vertices.resize(model.skin.rest.size());
    for (std::size_t v = 0; v < vertices.size(); ++v) vertices[v] = skinVertex(model, draws, v);
}
} // namespace smg
