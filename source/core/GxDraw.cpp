// The Wii GPU's state, read through the SDK's own register headers (private/*_reg.h, xf_mem.h), turned into what the
// PICA can draw: a TEV program for compileTev, the pixel tests, and triangles transformed through XF on the CPU.
#include <smg/GxDraw.hpp>
#include <private/gen_reg.h>
#include <private/pe_reg.h>
#include <private/ras_reg.h>
#include <private/su_reg.h>
#include <private/tev_reg.h>
#include <private/tx_reg.h>
#include <private/xf_mem.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace smg {
namespace {
float asFloat(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}
// `rows` rows of four words of XF memory from matrix row `row`, as floats; the index wraps as GxVertexPipeline::matrix's does.
void copyRows(float* to, const std::uint32_t* xf, std::uint32_t row, unsigned rows) {
    const std::uint32_t first = row * 4 & 0xFFF;
    if (first + rows * 4 <= 0x1000) return void(std::memcpy(to, xf + first, rows * 16));
    for (unsigned i = 0; i < rows * 4; ++i) to[i] = asFloat(xf[(row * 4 + i) & 0xFFF]);
}
// A TEV color register's 11-bit two's complement component, over 255.
float signed11(std::uint32_t value) { return static_cast<float>(static_cast<std::int32_t>(value << 21) >> 21) / 255.0f; }

// XF registers and memory, as the SDK's GX writes them.
constexpr std::uint32_t XfNumChans = 0x1009, XfAmbient0 = 0x100A, XfMaterial0 = 0x100C, XfColor0Ctrl = 0x100E, XfAlpha0Ctrl = 0x1010,
                        XfDualTex = 0x1012, XfMatIndexA = 0x1018, XfMatIndexB = 0x1019, XfViewport = 0x101A, XfProjection = 0x1020,
                        XfNumTexGens = 0x103F, XfTexGen = 0x1040, XfPostTexGen = 0x1050, XfPostMatrices = 0x500;

// Decoded op, bias and scale as the GX API numbers them (GXTevOp, GXTevBias, GXTevScale): a bias of 3 is a compare.
void decodeOp(std::uint32_t bias, std::uint32_t sub, std::uint32_t shift, std::uint8_t& op, std::uint8_t& outBias, std::uint8_t& scale) {
    if (bias == 3) {
        op = static_cast<std::uint8_t>(8 | shift << 1 | sub);
        outBias = scale = 0;
    } else {
        op = static_cast<std::uint8_t>(sub);
        outBias = static_cast<std::uint8_t>(bias);
        scale = static_cast<std::uint8_t>(shift);
    }
}

// The TEV order's channel as GXChannelID numbers it (GXTev.c's c2r, backwards).
std::uint8_t channelId(std::uint32_t hardware) {
    switch (hardware) {
    case 0: return 4;   // GX_COLOR0A0
    case 1: return 5;   // GX_COLOR1A1
    case 5: return 7;   // GX_ALPHA_BUMP
    case 6: return 8;   // GX_ALPHA_BUMPN
    default: return 6;  // GX_COLOR_ZERO
    }
}

void readTev(const GxCommandProcessor& gpu, GxDrawState& out) {
    ModelMaterial& m = out.material;
    const std::uint32_t gen = gpu.bp[0x00];
    m.tevStageCount = static_cast<std::uint8_t>(GEN_MODE_GET_NTEV(gen) + 1);
    m.texGenCount = static_cast<std::uint8_t>(GEN_MODE_GET_NTEX(gen));
    m.channelCount = static_cast<std::uint8_t>(GEN_MODE_GET_NCOL(gen));
    for (int i = 0; i < m.tevStageCount; ++i) {
        TevStage& s = m.stages[i];
        const std::uint32_t color = gpu.bp[0xC0 + 2 * i], alpha = gpu.bp[0xC1 + 2 * i];
        s.colorIn[0] = TEV_COLOR_ENV_GET_SELA(color), s.colorIn[1] = TEV_COLOR_ENV_GET_SELB(color);
        s.colorIn[2] = TEV_COLOR_ENV_GET_SELC(color), s.colorIn[3] = TEV_COLOR_ENV_GET_SELD(color);
        decodeOp(TEV_COLOR_ENV_GET_BIAS(color), TEV_COLOR_ENV_GET_SUB(color), TEV_COLOR_ENV_GET_SHIFT(color), s.colorOp, s.colorBias, s.colorScale);
        s.colorClamp = TEV_COLOR_ENV_GET_CLAMP(color), s.colorReg = TEV_COLOR_ENV_GET_DEST(color);
        s.alphaIn[0] = TEV_ALPHA_ENV_GET_SELA(alpha), s.alphaIn[1] = TEV_ALPHA_ENV_GET_SELB(alpha);
        s.alphaIn[2] = TEV_ALPHA_ENV_GET_SELC(alpha), s.alphaIn[3] = TEV_ALPHA_ENV_GET_SELD(alpha);
        decodeOp(TEV_ALPHA_ENV_GET_BIAS(alpha), TEV_ALPHA_ENV_GET_SUB(alpha), TEV_ALPHA_ENV_GET_SHIFT(alpha), s.alphaOp, s.alphaBias, s.alphaScale);
        s.alphaClamp = TEV_ALPHA_ENV_GET_CLAMP(alpha), s.alphaReg = TEV_ALPHA_ENV_GET_DEST(alpha);
        s.rasSwap = TEV_ALPHA_ENV_GET_MODE(alpha), s.texSwap = TEV_ALPHA_ENV_GET_SWAP(alpha);
        const std::uint32_t order = gpu.bp[0x28 + i / 2], ksel = gpu.bp[0xF6 + i / 2];
        const bool odd = i & 1;
        const bool enabled = odd ? RAS1_TREF_GET_TE1(order) : RAS1_TREF_GET_TE0(order);
        s.texMap = enabled ? static_cast<std::uint8_t>(odd ? RAS1_TREF_GET_TI1(order) : RAS1_TREF_GET_TI0(order)) : 0xFF;
        s.texCoord = static_cast<std::uint8_t>(odd ? RAS1_TREF_GET_TC1(order) : RAS1_TREF_GET_TC0(order));
        s.channel = channelId(odd ? RAS1_TREF_GET_CC1(order) : RAS1_TREF_GET_CC0(order));
        s.konstColor = static_cast<std::uint8_t>(odd ? TEV_KSEL_GET_KCSEL1(ksel) : TEV_KSEL_GET_KCSEL0(ksel));
        s.konstAlpha = static_cast<std::uint8_t>(odd ? TEV_KSEL_GET_KASEL1(ksel) : TEV_KSEL_GET_KASEL0(ksel));
        // BP 0x10+: bias 4-6, bump alpha 7-8, matrix 9-12, S and T wraps 13-18, LOD 19, add the previous stage's 20.
        const std::uint32_t indirect = gpu.bp[0x10 + i];
        if ((indirect >> 13 & 0x3F) == (6 | 6 << 3) && !(indirect >> 7 & 3) && !(indirect >> 20 & 1))  // GX_ITW_0 both ways
            out.leftOut = "a draw whose coordinates an indirect lookup replaces (a dependent read)";
        else if (indirect >> 7 & 3 || indirect >> 13 & 0x3F || indirect >> 20 & 1)
            out.unsupported = "an indirect lookup that wraps, replaces or chains its coordinates, or bumps alpha";
        else if (indirect >> 9 & 0xF) out.indirectDropped = true;
        if (s.texMap < 8) m.textures[s.texMap] = s.texMap;
    }
    for (int r = 0; r < 4; ++r) {
        // A material keeps C0, C1, C2 then PREV (as J3D's MAT3 does); BP 0xE0 holds PREV, 0xE2 C0 and so on.
        const int pair = (r + 1) % 4;
        const std::uint32_t ra = gpu.tevColor[2 * pair], bg = gpu.tevColor[2 * pair + 1];
        m.tevColors[r] = {signed11(TEV_REGISTERL_GET_R(ra)), signed11(TEV_REGISTERH_GET_G(bg)), signed11(TEV_REGISTERH_GET_B(bg)),
                          signed11(TEV_REGISTERL_GET_A(ra))};
        const std::uint32_t kra = gpu.tevKonst[2 * r], kbg = gpu.tevKonst[2 * r + 1];
        m.konstColors[r] = {TEV_KREGISTERL_GET_R(kra) / 255.0f, TEV_KREGISTERH_GET_G(kbg) / 255.0f, TEV_KREGISTERH_GET_B(kbg) / 255.0f,
                            TEV_KREGISTERL_GET_A(kra) / 255.0f};
        const std::uint32_t rb = gpu.bp[0xF6 + 2 * r], ga = gpu.bp[0xF7 + 2 * r];
        m.swapTables[r][0] = static_cast<std::uint8_t>(TEV_KSEL_GET_XRB(rb)), m.swapTables[r][1] = static_cast<std::uint8_t>(TEV_KSEL_GET_XGA(rb));
        m.swapTables[r][2] = static_cast<std::uint8_t>(TEV_KSEL_GET_XRB(ga)), m.swapTables[r][3] = static_cast<std::uint8_t>(TEV_KSEL_GET_XGA(ga));
    }
}

void readPixel(const GxCommandProcessor& gpu, GxDrawState& out) {
    ModelMaterial& m = out.material;
    const std::uint32_t alpha = gpu.bp[0xF3];
    m.alphaTest.reference0 = static_cast<std::uint8_t>(TEV_ALPHAFUNC_GET_A0(alpha)), m.alphaTest.reference1 = static_cast<std::uint8_t>(TEV_ALPHAFUNC_GET_A1(alpha));
    m.alphaTest.compare0 = static_cast<std::uint8_t>(TEV_ALPHAFUNC_GET_OP0(alpha)), m.alphaTest.compare1 = static_cast<std::uint8_t>(TEV_ALPHAFUNC_GET_OP1(alpha));
    m.alphaTest.op = static_cast<std::uint8_t>(TEV_ALPHAFUNC_GET_LOGIC(alpha));
    const std::uint32_t blend = gpu.bp[0x41];
    // GXBlendMode: GX_BM_NONE, BLEND, LOGIC, SUBTRACT, as GXSetBlendMode sets the enables.
    m.blend.type = PE_CMODE0_GET_BLENDOP(blend) ? 3 : PE_CMODE0_GET_BLEND_ENABLE(blend) ? 1 : PE_CMODE0_GET_LOGICOP_ENABLE(blend) ? 2 : 0;
    m.blend.source = static_cast<std::uint8_t>(PE_CMODE0_GET_SFACTOR(blend)), m.blend.destination = static_cast<std::uint8_t>(PE_CMODE0_GET_DFACTOR(blend));
    m.blend.logic = static_cast<std::uint8_t>(PE_CMODE0_GET_LOGICOP(blend));
    out.colorUpdate = PE_CMODE0_GET_COLOR_MASK(blend), out.alphaUpdate = PE_CMODE0_GET_ALPHA_MASK(blend);
    out.dstAlphaEnable = gpu.bp[0x42] >> PE_CMODE1_CONSTANT_ALPHA_ENABLE_SHIFT & 1;
    out.dstAlpha = static_cast<std::uint8_t>(PE_CMODE1_GET_CONSTANT_ALPHA(gpu.bp[0x42]));
    const std::uint32_t depth = gpu.bp[0x40];
    m.depth.enable = PE_ZMODE_GET_ENABLE(depth), m.depth.function = static_cast<std::uint8_t>(PE_ZMODE_GET_FUNC(depth));
    m.depth.update = PE_ZMODE_GET_MASK(depth);
    m.cullMode = 0;  // culled here, in EFB space
    out.depthTextureOp = static_cast<std::uint8_t>(TEV_Z_ENV_1_GET_OP(gpu.bp[0xF5]));
    out.depthTextureType = static_cast<std::uint8_t>(TEV_Z_ENV_1_GET_TYPE(gpu.bp[0xF5]));
    out.depthTextureOffset = TEV_Z_ENV_0_GET_ZOFF(gpu.bp[0xF4]);
    // GXSetScissor adds 342 to each edge; right and bottom are inclusive.
    out.scissor[0] = static_cast<int>(SU_SCIS0_GET_SX0(gpu.bp[0x20])) - 342, out.scissor[1] = static_cast<int>(SU_SCIS0_GET_SY0(gpu.bp[0x20])) - 342;
    out.scissor[2] = static_cast<int>(SU_SCIS1_GET_SX1(gpu.bp[0x21])) - 341, out.scissor[3] = static_cast<int>(SU_SCIS1_GET_SY1(gpu.bp[0x21])) - 341;
    for (int map = 0; map < 8; ++map) {
        const std::uint32_t bank = map < 4 ? 0x80 + map : 0xA0 + map - 4;
        const std::uint32_t mode = gpu.bp[bank], image0 = gpu.bp[bank + 0x08], image3 = gpu.bp[bank + 0x14];
        GxTextureMap& t = out.textures[map];
        t.width = static_cast<std::uint16_t>(TX_SETIMAGE0_GET_IMAGE_WIDTH(image0) + 1);
        t.height = static_cast<std::uint16_t>(TX_SETIMAGE0_GET_IMAGE_HEIGHT(image0) + 1);
        t.format = static_cast<std::uint8_t>(TX_SETIMAGE0_GET_FORMAT(image0));
        t.address = TX_SETIMAGE3_GET_IMAGE_BASE(image3) << 5;
        t.wrapS = static_cast<std::uint8_t>(TX_SETMODE0_GET_WRAP_S(mode)), t.wrapT = static_cast<std::uint8_t>(TX_SETMODE0_GET_WRAP_T(mode));
        t.magFilter = static_cast<std::uint8_t>(TX_SETMODE0_GET_MAG_FILTER(mode)), t.minFilter = static_cast<std::uint8_t>(TX_SETMODE0_GET_MIN_FILTER(mode));
        t.maxLod = static_cast<std::uint8_t>(TX_SETMODE1_GET_MAXLOD(gpu.bp[bank + 0x04])), t.lodBias = static_cast<std::uint8_t>(TX_SETMODE0_GET_LODBIAS(mode));
    }
}

float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool normalize(float v[3]) {
    const float length = std::sqrt(dot(v, v));
    if (length == 0) return false;
    for (int i = 0; i < 3; ++i) v[i] /= length;
    return true;
}
float safeDivide(float n, float d) { return d == 0 ? (n > 0 ? 3.4e38f : 0) : n / d; }

constexpr std::uint32_t XfLights = 0x600;

// One light's contribution, scaled by its attenuation (specular or spot, by the control word) and turned by the normal
// (none, signed or clamped), into the components [first, last) of `sum` (0-255 each).
void addLight(const GxLighting::Light& l, const GxLighting::Part& part, const float view[3], const float normal[3], float sum[4], int first, int last,
              float* sumAlpha) {
    float distance[3] = {l.distance[0], l.distance[1], l.distance[2]};
    float toLight[3] = {l.position[0] - view[0], l.position[1] - view[1], l.position[2] - view[2]};
    float scale = 1;
    if (part.attenuated && part.spot) {
        const float d2 = dot(toLight, toLight), d = std::sqrt(d2);
        if (d > 0) for (float& k : toLight) k /= d;
        const float angle = std::max(0.0f, dot(toLight, l.direction));
        scale = safeDivide(std::max(0.0f, l.cosine[0] + l.cosine[1] * angle + l.cosine[2] * angle * angle),
                           distance[0] + distance[1] * d + distance[2] * d2);
    } else if (part.attenuated) {
        // Specular: the light's direction holds the half-angle, and distance attenuation runs on the same angle.
        normalize(toLight);
        const float angle = dot(toLight, normal) >= 0 ? std::max(0.0f, dot(l.direction, normal)) : 0.0f;
        if (part.diffuse) normalize(distance);
        scale = safeDivide(std::max(0.0f, l.cosine[0] + l.cosine[1] * angle + l.cosine[2] * angle * angle),
                           distance[0] + distance[1] * angle + distance[2] * angle * angle);
    } else if (!normalize(toLight)) {
        std::memcpy(toLight, normal, sizeof toLight);
    }
    float turn = 1;
    if (part.diffuse) turn = part.diffuse == 2 ? std::max(0.0f, dot(toLight, normal)) : dot(toLight, normal);
    for (int c = first; c < last; ++c) sum[c] += l.color[c] * scale * turn;
    if (sumAlpha) *sumAlpha += l.color[3] * scale * turn;
}
} // namespace

void GxLighting::read(const GxCommandProcessor& gpu) {
    const std::uint32_t ctrl[2] = {gpu.xf[XfColor0Ctrl], gpu.xf[XfAlpha0Ctrl]}, materialWord = gpu.xf[XfMaterial0], ambientWord = gpu.xf[XfAmbient0];
    for (int c = 0; c < 4; ++c)
        material[c] = static_cast<std::uint8_t>(materialWord >> (24 - 8 * c)), ambient[c] = static_cast<std::uint8_t>(ambientWord >> (24 - 8 * c));
    std::uint8_t used = 0;
    for (int i = 0; i < 2; ++i) {
        Part& part = parts[i];
        const std::uint32_t w = ctrl[i];
        part.lit = XF_COLOR0CNTRL_F_GET_LIGHTFUNC(w);
        part.materialFromVertex = XF_COLOR0CNTRL_F_GET_MATERIAL_SRC(w), part.ambientFromVertex = XF_COLOR0CNTRL_F_GET_AMBIENT_SRC(w);
        part.mask = static_cast<std::uint8_t>(XF_COLOR0CNTRL_F_GET_LIGHT0(w) | XF_COLOR0CNTRL_F_GET_LIGHT1(w) << 1 | XF_COLOR0CNTRL_F_GET_LIGHT2(w) << 2 |
                                              XF_COLOR0CNTRL_F_GET_LIGHT3(w) << 3 | XF_COLOR0CNTRL_F_GET_LIGHT4(w) << 4 | XF_COLOR0CNTRL_F_GET_LIGHT5(w) << 5 |
                                              XF_COLOR0CNTRL_F_GET_LIGHT6(w) << 6 | XF_COLOR0CNTRL_F_GET_LIGHT7(w) << 7);
        part.diffuse = static_cast<std::uint8_t>(XF_COLOR0CNTRL_F_GET_DIFFUSEATTEN(w));  // GXDiffuseFn: none, sign, clamp
        part.attenuated = XF_COLOR0CNTRL_F_GET_ATTENENABLE(w), part.spot = XF_COLOR0CNTRL_F_GET_ATTENSELECT(w);
        if (part.lit) used |= part.mask;
        lightCount[i] = 0;
        for (int n = 0; n < 8; ++n)
            if (part.mask >> n & 1) lightsOf[i][lightCount[i]++] = static_cast<std::uint8_t>(n);
    }
    shared = parts[0].lit && parts[1].lit && parts[0].mask == parts[1].mask && parts[0].diffuse == parts[1].diffuse &&
             parts[0].attenuated == parts[1].attenuated && parts[0].spot == parts[1].spot;
    for (int n = 0; n < 8; ++n) {
        if (!(used >> n & 1)) continue;
        const std::uint32_t* w = gpu.xf + XfLights + 16 * n;  // three reserved words, color, A0-A2, K0-K2, position, direction
        Light& l = lights[n];
        for (int c = 0; c < 4; ++c) l.color[c] = static_cast<float>(w[3] >> (24 - 8 * c) & 0xFF);
        for (int k = 0; k < 3; ++k)
            l.cosine[k] = asFloat(w[4 + k]), l.distance[k] = asFloat(w[7 + k]), l.position[k] = asFloat(w[10 + k]), l.direction[k] = asFloat(w[13 + k]);
    }
}

void GxLighting::apply(const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]) const {
    float sums[2][4];
    for (int i = 0; i < 2; ++i) {
        if (parts[i].ambientFromVertex)
            for (int c = 0; c < 4; ++c) sums[i][c] = vertex[c];
        else
            for (int c = 0; c < 4; ++c) sums[i][c] = ambient[c];
    }
    if (shared) {
        for (unsigned k = 0; k < lightCount[0]; ++k) addLight(lights[lightsOf[0][k]], parts[0], view, normal, sums[0], 0, 3, &sums[1][3]);
    } else {
        for (int i = 0; i < 2; ++i)
            if (parts[i].lit)
                for (unsigned k = 0; k < lightCount[i]; ++k) addLight(lights[lightsOf[i][k]], parts[i], view, normal, sums[i], i ? 3 : 0, i ? 4 : 3, nullptr);
    }
    for (int i = 0; i < 2; ++i) {
        // Part 0 is the color (RGB), part 1 the alpha.
        const Part& part = parts[i];
        for (int c = i ? 3 : 0; c < (i ? 4 : 3); ++c) {
            const std::uint8_t mat = part.materialFromVertex ? vertex[c] : material[c];
            if (!part.lit) {
                out[c] = mat;
                continue;
            }
            const int light = std::clamp(static_cast<int>(sums[i][c]), 0, 255);
            out[c] = static_cast<std::uint8_t>(mat * (light + (light >> 7)) >> 8);
        }
    }
}

GxMeshLayout GxMeshLayout::make(bool colorFromVertex, bool lit, bool slot, unsigned units, const Texgen* texgens) {
    GxMeshLayout layout;
    layout.lit = lit;
    unsigned at = 12;
    if (colorFromVertex) layout.color = static_cast<std::uint8_t>(at), at += 4;
    bool normal = lit;
    for (unsigned u = 0; u < units; ++u) {
        Unit& unit = layout.unit[u];
        const Texgen& t = texgens[u];
        const bool geometry = t.source <= 1;
        unit.source = t.source;
        // Only a position or normal read with three inputs has a third of its own, else it is one; a slot texgen's rows read all three.
        if (geometry && t.threeInputs) unit.input = t.source == 0 ? OfPosition : OfNormal, unit.floats = 3;
        else if (!t.perVertexMatrix) unit.input = !geometry ? Stored : t.source == 0 ? OfPosition : OfNormal, unit.floats = 2;
        else unit.input = Stored, unit.floats = 3;
        normal |= unit.input == OfNormal;
        if (unit.input == Stored) unit.at = static_cast<std::uint8_t>(at), at += 4u * unit.floats;
    }
    if (normal) layout.normal = static_cast<std::uint8_t>(at), at += 12;
    if (slot) layout.slot = static_cast<std::uint8_t>(at), at += 4;
    layout.stride = static_cast<std::uint8_t>(at);
    return layout;
}

const char* GxMeshLayout::fieldAt(unsigned byte) const {
    if (byte < 12) return "position";
    if (color != Absent && byte - color < 4) return "color";
    if (normal != Absent && byte - normal < 12) return "normal";
    if (slot != Absent && byte - slot < 4) return "slot";
    return "texgen inputs";
}

GxMeshPacker::GxMeshPacker(const GxMeshLayout& layout) : stride_(layout.stride) {
    const auto add = [&](std::size_t from, unsigned to, unsigned size, bool one) {
        copies_[count_++] = {static_cast<std::uint16_t>(from), static_cast<std::uint8_t>(to), static_cast<std::uint8_t>(size), one};
    };
    add(offsetof(GxVertex, position), 0, 12, false);
    if (layout.color != GxMeshLayout::Absent) add(offsetof(GxVertex, color), layout.color, 4, false);
    for (const GxMeshLayout::Unit& unit : layout.unit) {
        if (unit.input != GxMeshLayout::Stored) continue;
        const std::size_t from = unit.source == 0   ? offsetof(GxVertex, position)
                                 : unit.source == 1 ? offsetof(GxVertex, normal)
                                                    : offsetof(GxVertex, texcoord) + 8 * (unit.source - 5);
        add(from, unit.at, 8, unit.floats == 3);  // two components of the source, then a one for the third
    }
    if (layout.normal != GxMeshLayout::Absent) add(offsetof(GxVertex, normal), layout.normal, 12, false);
}

void GxMeshPacker::pack(const GxVertex* v, std::size_t count, std::uint8_t* out) const {
    static constexpr float One = 1;
    for (std::size_t i = 0; i < count; ++i, ++v, out += stride_) {
        const std::uint8_t* const from = reinterpret_cast<const std::uint8_t*>(v);
        for (unsigned c = 0; c < count_; ++c) {
            const Copy& copy = copies_[c];
            if (copy.size == 12) std::memcpy(out + copy.to, from + copy.from, 12);
            else if (copy.size == 8) std::memcpy(out + copy.to, from + copy.from, 8);
            else std::memcpy(out + copy.to, from + copy.from, 4);
            if (copy.one) std::memcpy(out + copy.to + 8, &One, 4);
        }
    }
}

GxMeshVertex gxUnpackMeshVertex(const GxMeshLayout& layout, const std::uint8_t* packed) {
    GxMeshVertex v{};
    std::memcpy(v.position, packed, sizeof v.position);
    if (layout.color != GxMeshLayout::Absent) std::memcpy(v.color, packed + layout.color, 4);
    if (layout.normal != GxMeshLayout::Absent) std::memcpy(v.normal, packed + layout.normal, sizeof v.normal);
    for (unsigned u = 0; u < CombinerProgram::MaxUnits; ++u) {
        const GxMeshLayout::Unit& unit = layout.unit[u];
        if (unit.input == GxMeshLayout::Unused) continue;
        const std::uint8_t* from = packed;
        if (unit.input == GxMeshLayout::Stored) from += unit.at;
        else if (unit.input == GxMeshLayout::OfNormal) from += layout.normal;
        std::memcpy(v.source[u], from, unit.floats * 4u);
    }
    if (layout.slot != GxMeshLayout::Absent) std::memcpy(&v.slot, packed + layout.slot, sizeof v.slot);
    else v.slot = layout.lit ? 4.0f : 0.0f;
    return v;
}

void gxMeshModel(const GxMeshUniforms& u, const GxMeshVertex& v, float clip[4], float color[4], float tex[CombinerProgram::MaxUnits][2]) {
    const auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto dot4 = [](const float* row, const float* xyz) { return row[0] * xyz[0] + row[1] * xyz[1] + row[2] * xyz[2] + row[3]; };
    const unsigned slot = u.lit ? (static_cast<unsigned>(v.slot) - 4) / u.slotRows : static_cast<unsigned>(v.slot) / 4;
    float view[3];
    if (u.lit) {
        // The slot's modelview, then the shared rows to clip space, as the shader does.
        for (int r = 0; r < 3; ++r) view[r] = dot4(u.modelView[slot] + r * 4, v.position);
        for (int r = 0; r < 4; ++r) clip[r] = dot4(u.toClip + r * 4, view);
    } else {
        for (int r = 0; r < 4; ++r) clip[r] = dot4(u.clip[slot] + r * 4, v.position);
    }
    for (unsigned t = 0; t < u.units; ++t) {
        float in[3] = {v.source[t][0], v.source[t][1], v.source[t][2]};
        if (u.slotTexgen[t]) {
            const float* rows = u.slotTexgen[t] == GxMesh::SlotModelView ? u.modelView[slot] : u.paired[slot][(static_cast<int>(u.slotTexgenRow[t]) - 6) / 3];
            for (int r = 0; r < 3; ++r) in[r] = dot4(rows + r * 4, v.source[t]);
        }
        const float s = dot4(u.texgen[t][0], in), w = dot4(u.texgen[t][1], in);
        const float q = u.projective[t] ? dot4(u.texgen[t][2], in) : 1;
        tex[t][0] = s / q, tex[t][1] = w / q;
    }
    for (int c = 0; c < 4; ++c) {
        const bool fromRegister = !u.fixedColor && (c < 3 ? !u.rgbFromVertex : !u.alphaFromVertex);
        color[c] = (u.fixedColor || fromRegister ? u.color[c] : v.color[c]) / 255.0f;
    }
    if (!u.lit) return;
    const float* nm = u.normal[slot];
    float n[3];
    for (int r = 0; r < 3; ++r) n[r] = dot3(nm + r * 3, v.normal);
    const float nl = dot3(n, n) > 0 ? 1 / std::sqrt(dot3(n, n)) : 0;
    for (float& c : n) c *= nl;
    float sum[4] = {u.ambient[0], u.ambient[1], u.ambient[2], u.ambient[3]};
    for (int c = 0; c < 4; ++c)
        if (c < 3 ? u.rgbAmbientFromVertex : u.alphaAmbientFromVertex) sum[c] = v.color[c] / 255.0f;
    for (unsigned l = 0; l < u.lights; ++l) {
        const float(&L)[6][4] = u.light[l];
        float t[3];
        for (int c = 0; c < 3; ++c) t[c] = view[c] * L[4][0] + L[1][c];
        const float len2 = dot3(t, t), inv = len2 > 0 ? 1 / std::sqrt(len2) : 0;
        for (float& c : t) c *= inv;
        const float spotAngle = std::max(0.0f, dot3(t, L[2])), distance = len2 * inv * L[4][1];
        const float specular = dot3(t, n) >= 0 ? std::max(0.0f, dot3(L[2], n)) : 0.0f;
        const float a = specular + L[5][0] * (spotAngle - specular), d = specular + L[5][0] * (distance - specular);
        float scale = std::max(0.0f, (a * L[3][0] + L[2][3]) * a + L[1][3]) / ((d * L[3][3] + L[3][2]) * d + L[3][1]);
        scale *= 1 + L[5][1] * (std::max(dot3(t, n), L[5][2]) - 1);
        for (int c = 0; c < 4; ++c) sum[c] += L[0][c] * scale;
    }
    for (int c = 0; c < 4; ++c)
        if (c < 3 ? u.rgbLit : u.alphaLit) color[c] *= std::clamp(sum[c], 0.0f, 1.0f);
}

void gxLitChannel(const GxCommandProcessor& gpu, const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]) {
    GxLighting lighting;
    lighting.read(gpu);
    lighting.apply(view, normal, vertex, out);
}

namespace {
// compileTev reads only these; a scene's draws repeat a few hundred of them, so each is compiled once. Zeroed whole, so
// two keys compare (and hash) by their bytes, and only as far as the stages in use.
struct TevKey {
    std::array<float, 4> tevColors[4], konstColors[4];
    int textures[8];
    std::uint8_t swapTables[4][4];
    std::uint8_t color1FromUnit;
    std::uint8_t grayMaps;
    std::uint8_t count;
    TevStage stages[16];
};

std::uint32_t tevCompiles = 0;

const CombinerProgram& cachedTev(const ModelMaterial& m, bool color1FromUnit = false, std::uint8_t grayMaps = 0) {
    TevKey key;
    std::memset(static_cast<void*>(&key), 0, sizeof key);
    key.color1FromUnit = color1FromUnit;
    key.grayMaps = grayMaps;
    key.count = std::min<std::uint8_t>(m.tevStageCount, 16);
    std::copy(m.stages, m.stages + key.count, key.stages);
    std::copy(m.tevColors, m.tevColors + 4, key.tevColors);
    std::copy(m.konstColors, m.konstColors + 4, key.konstColors);
    std::memcpy(key.swapTables, m.swapTables, sizeof key.swapTables);
    std::copy(m.textures, m.textures + 8, key.textures);
    const std::size_t length = offsetof(TevKey, stages) + key.count * sizeof(TevStage);
    // By words (the zeroed key's tail rounds up harmlessly), with a 32-bit multiply: the ARM11 has no 64-bit one.
    std::uint32_t hash = 0x811C9DC5u;
    for (std::size_t i = 0; i < length; i += 4) {
        std::uint32_t word;
        std::memcpy(&word, reinterpret_cast<const std::uint8_t*>(&key) + i, 4);
        hash = (hash ^ word) * 0x01000193u;
    }
    struct Entry {
        TevKey key;
        CombinerProgram program;
    };
    thread_local std::unordered_map<std::uint32_t, Entry> cache;
    if (auto found = cache.find(hash); found != cache.end() && !std::memcmp(&found->second.key, &key, length)) return found->second.program;
    if (cache.size() > 4096) cache.clear();
    ++tevCompiles;
    Entry& entry = cache[hash];
    entry.key = key;
    entry.program = compileTev(m, color1FromUnit, grayMaps);
    return entry.program;
}
} // namespace

std::uint32_t gxTevCompiles() { return tevCompiles; }

namespace {
// TEV register `reg` (1 C0, 2 C1) as four bytes: BP 0xE0 + 2 reg and the next, eleven-bit two's complement components.
bool tevColorBytes(const GxCommandProcessor& gpu, unsigned reg, std::uint8_t out[4]) {
    const std::uint32_t ra = gpu.tevColor[2 * reg], bg = gpu.tevColor[2 * reg + 1];
    // A component is a byte when its top three bits are clear (R and A in ra, B and G in bg).
    if ((ra | bg) & 0x700700) return false;
    out[0] = static_cast<std::uint8_t>(ra), out[1] = static_cast<std::uint8_t>(bg >> 12);
    out[2] = static_cast<std::uint8_t>(bg), out[3] = static_cast<std::uint8_t>(ra >> 12);
    return true;
}
} // namespace

bool gxColor0Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]) { return tevColorBytes(gpu, 1, out); }
bool gxColor1Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]) { return tevColorBytes(gpu, 2, out); }

namespace {
// C0 can ride in the vertex color when the program reads it and never writes it, never reads the rasterized color
// (whose place it takes), C0 is a byte color, and a swap table leaves the rasterized color as it is.
bool color0CanRide(const GxCommandProcessor& gpu, const ModelMaterial& m, std::uint8_t& identitySwap) {
    bool reads = false;
    for (int i = 0; i < m.tevStageCount; ++i) {
        const TevStage& s = m.stages[i];
        for (int k = 0; k < 4; ++k) {
            if (s.colorIn[k] == 10 || s.colorIn[k] == 11 || s.alphaIn[k] == 5) return false;  // RASC, RASA
            reads |= s.colorIn[k] == 2 || s.colorIn[k] == 3 || s.alphaIn[k] == 1;            // C0, A0
        }
        if (s.colorReg == 1 || s.alphaReg == 1) return false;  // GX_TEVREG0
    }
    std::uint8_t bytes[4];
    if (!reads || !gxColor0Bytes(gpu, bytes)) return false;
    for (std::uint8_t t = 0; t < 4; ++t) {
        const std::uint8_t* table = m.swapTables[t];
        if (table[0] == 0 && table[1] == 1 && table[2] == 2 && table[3] == 3) return identitySwap = t, true;
    }
    return false;
}

// C1 can come from a palette texel when the program reads it, no stage writes it and it fits in bytes.
bool color1CanRide(const GxCommandProcessor& gpu, const ModelMaterial& m) {
    bool reads = false;
    for (int i = 0; i < m.tevStageCount; ++i) {
        const TevStage& s = m.stages[i];
        for (int k = 0; k < 4; ++k) reads |= s.colorIn[k] == 4 || s.colorIn[k] == 5 || s.alphaIn[k] == 2;  // C1, A1
        if (s.colorReg == 2 || s.alphaReg == 2) return false;                                               // GX_TEVREG1
    }
    std::uint8_t bytes[4];
    return reads && gxColor1Bytes(gpu, bytes);
}
} // namespace

namespace {
// What a TEV stage input reads, by its selection: bits 0-3 the register (PREV, C0, C1, C2), then the konst and the rasterized color.
constexpr std::uint8_t ReadsKonst = 0x10, ReadsRaster = 0x20;
constexpr std::array<std::uint8_t, 16> colorReads = [] {
    std::array<std::uint8_t, 16> reads{};
    for (unsigned in = 0; in < 16; ++in) reads[in] = in < 8 ? 1u << in / 2 : in == 14 ? ReadsKonst : in == 10 || in == 11 ? ReadsRaster : 0;
    return reads;
}();
constexpr std::array<std::uint8_t, 8> alphaReads = [] {
    std::array<std::uint8_t, 8> reads{};
    for (unsigned in = 0; in < 8; ++in) reads[in] = in < 4 ? 1u << in : in == 6 ? ReadsKonst : in == 5 ? ReadsRaster : 0;
    return reads;
}();
} // namespace

std::size_t gxDrawStateKey(const GxCommandProcessor& gpu, std::uint32_t* out, bool palette, unsigned* markers, std::size_t* mapsAt) {
    std::uint32_t* at = out;
    const std::uint32_t gen = gpu.bp[0x00];
    *at++ = gen;
    for (std::uint32_t a : {0x20u, 0x21u, 0x40u, 0x41u, 0x42u, 0xF3u, 0xF4u, 0xF5u}) *at++ = gpu.bp[a];
    at[-5] &= ~std::uint32_t(PE_CMODE0_DITHER_ENABLE_MASK);  // the blend mode's dither, which the PICA has no switch for
    for (std::uint32_t a = 0xF6; a <= 0xFD; ++a) *at++ = gpu.bp[a];  // konst selects and swap tables
    for (std::uint32_t x : {0x1009u, 0x100Au, 0x100Cu, 0x100Eu, 0x1010u, 0x103Fu}) *at++ = gpu.xf[x];
    const unsigned stages = GEN_MODE_GET_NTEV(gen) + 1;
    // Only the registers the stages read: values earlier draws left in the others would split one state into many.
    unsigned reads = 0, written = 0;
    for (unsigned i = 0; i < stages; ++i) {
        const std::uint32_t color = gpu.bp[0xC0 + 2 * i], alpha = gpu.bp[0xC1 + 2 * i];
        reads |= colorReads[TEV_COLOR_ENV_GET_SELA(color)] | colorReads[TEV_COLOR_ENV_GET_SELB(color)] | colorReads[TEV_COLOR_ENV_GET_SELC(color)] |
                 colorReads[TEV_COLOR_ENV_GET_SELD(color)] | alphaReads[TEV_ALPHA_ENV_GET_SELA(alpha)] | alphaReads[TEV_ALPHA_ENV_GET_SELB(alpha)] |
                 alphaReads[TEV_ALPHA_ENV_GET_SELC(alpha)] | alphaReads[TEV_ALPHA_ENV_GET_SELD(alpha)];
        written |= 1u << TEV_COLOR_ENV_GET_DEST(color) | 1u << TEV_ALPHA_ENV_GET_DEST(alpha);
    }
    const unsigned registers = reads & 0xF;
    const bool konst = reads & ReadsKonst, raster = reads & ReadsRaster;
    // A C0 riding in the vertex color (color0CanRide), or a C1 in the palette (color1CanRide), is read where it is drawn:
    // the state is the same whatever its value, so a marker stands for it.
    std::uint8_t bytes[4];
    bool identity = false;
    for (unsigned t = 0; t < 4; ++t) {
        const std::uint32_t rb = gpu.bp[0xF6 + 2 * t], ga = gpu.bp[0xF7 + 2 * t];
        identity |= TEV_KSEL_GET_XRB(rb) == 0 && TEV_KSEL_GET_XGA(rb) == 1 && TEV_KSEL_GET_XRB(ga) == 2 && TEV_KSEL_GET_XGA(ga) == 3;
    }
    const bool c0Rides = (registers & 2) && !raster && !(written & 2) && identity && gxColor0Bytes(gpu, bytes);
    const bool c1Rides = palette && (registers & 4) && !(written & 4) && gxColor1Bytes(gpu, bytes);
    if (markers) *markers = (c0Rides ? 1 : 0) | (c1Rides ? 2 : 0);
    for (unsigned r = 0; r < 4; ++r) {
        if (!(registers >> r & 1)) continue;
        if ((r == 1 && c0Rides) || (r == 2 && c1Rides)) *at++ = 0xC0C0C0C0u;
        else *at++ = gpu.tevColor[2 * r], *at++ = gpu.tevColor[2 * r + 1];
    }
    if (konst)
        for (int i = 0; i < 8; ++i) *at++ = gpu.tevKonst[i];
    std::uint8_t maps = 0;
    for (unsigned i = 0; i < stages; ++i) {
        *at++ = gpu.bp[0xC0 + 2 * i], *at++ = gpu.bp[0xC1 + 2 * i], *at++ = gpu.bp[0x10 + i];
        const std::uint32_t order = gpu.bp[0x28 + i / 2];
        if (i % 2 == 0) *at++ = order;
        if (i & 1 ? RAS1_TREF_GET_TE1(order) : RAS1_TREF_GET_TE0(order)) maps |= 1u << (i & 1 ? RAS1_TREF_GET_TI1(order) : RAS1_TREF_GET_TI0(order));
    }
    if (mapsAt) *mapsAt = std::size_t(at - out);
    for (int map = 0; map < 8; ++map) {
        if (!(maps >> map & 1)) continue;
        const std::uint32_t bank = map < 4 ? 0x80 + map : 0xA0 + map - 4;
        *at++ = gpu.bp[bank], *at++ = gpu.bp[bank + 0x08], *at++ = gpu.bp[bank + 0x14];
    }
    return std::size_t(at - out);
}

namespace {
// The maps whose formats give equal red, green and blue (I4, I8, IA4 and IA8).
std::uint8_t grayMaps(const GxDrawState& state) {
    std::uint8_t maps = 0;
    for (int map = 0; map < 8; ++map) maps = static_cast<std::uint8_t>(maps | (state.textures[map].format <= 3) << map);
    return maps;
}

void readProgram(const GxCommandProcessor& gpu, GxDrawState& out, bool palette) {
    readTev(gpu, out);
    readPixel(gpu, out);
    std::uint8_t identitySwap = 0;
    if (!color0CanRide(gpu, out.material, identitySwap)) {
        out.program = cachedTev(out.material, false, grayMaps(out));
        return;
    }
    // Compiled with C0 read as the rasterized color and its value left out, so every C0 shares one program.
    ModelMaterial riding = out.material;
    riding.tevColors[0] = {0, 0, 0, 0};
    for (int i = 0; i < riding.tevStageCount; ++i) {
        TevStage& s = riding.stages[i];
        bool used = false;
        for (int k = 0; k < 4; ++k) {
            if (s.colorIn[k] == 2 || s.colorIn[k] == 3) s.colorIn[k] = static_cast<std::uint8_t>(s.colorIn[k] + 8), used = true;
            if (s.alphaIn[k] == 1) s.alphaIn[k] = 5, used = true;
        }
        if (used) s.channel = 4, s.rasSwap = identitySwap;  // GX_COLOR0A0
    }
    const CombinerProgram program = cachedTev(riding);  // a copy: the next lookup may empty the cache
    if (!program.exact && cachedTev(out.material).exact) return void(out.program = cachedTev(out.material));
    out.program = program;
    out.color0InVertex = true;
    if (!palette || !color1CanRide(gpu, riding)) return;
    riding.tevColors[1] = {0, 0, 0, 0};
    const CombinerProgram both = cachedTev(riding, true);
    if (!both.exact && program.exact) return;
    for (unsigned u = 0; u < both.unitCount; ++u)
        if (both.units[u].texture == CombinerProgram::Palette) out.program = both, out.color1InTexture = true, out.paletteUnit = static_cast<std::uint8_t>(u);
}

// A material one pass can't draw exactly is drawn in passes when it splits, each exact, with C0 and C1 read as constants.
// An alpha test of two compares, of which a draw keeps one, is drawn again under the other (the same program, as is).
void readPasses(GxDrawState& out) {
    const std::uint8_t gray = grayMaps(out);
    TevPasses split;
    if (out.program.exact || !splitTev(out.material, split, gray)) {
        if (!splitAlphaTest(out.material, split)) return;
        out.material.alphaTest = split.first.alphaTest;
        for (const TevPass& pass : split.extra) out.passes.push_back({out.program, pass.blend, pass.reference, false, true, pass.compare});
        return;
    }
    const CombinerProgram first = cachedTev(split.first, false, gray);
    if (!first.exact) return;
    std::vector<GxDrawState::Pass> passes;
    for (const TevPass& pass : split.extra) {
        const CombinerProgram& program = pass.hasProgram ? pass.program : cachedTev(pass.material, false, gray);
        if (!program.exact) return;
        bool same = program.unitCount <= first.unitCount;
        for (unsigned u = 0; same && u < program.unitCount; ++u)
            same = program.units[u].texture == first.units[u].texture && program.units[u].texCoord == first.units[u].texCoord;
        passes.push_back({program, pass.blend, pass.reference, pass.writesAlpha, same, pass.compare});
    }
    out.program = first, out.passes = std::move(passes), out.material.alphaTest = split.first.alphaTest;
    out.color0InVertex = out.color1InTexture = false, out.paletteUnit = 0;
}
} // namespace

void readGxDrawState(const GxCommandProcessor& gpu, GxDrawState& out, bool palette) {
    out = GxDrawState{};
    readProgram(gpu, out, palette);
    readPasses(out);
}

float GxVertexPipeline::matrix(std::uint32_t row, int column) const { return asFloat(gpu_->xf[(row * 4 + column) & 0xFFF]); }

bool GxVertexPipeline::prepare(const GxCommandProcessor& gpu, const GxDrawState& state, const GxFrame& frame, std::uint8_t kind, std::uint8_t format) {
    // Of the CP registers, only the descriptor's matrix-index bits are read; the kind and format are only kept.
    if (prepared_ && &gpu == gpu_ && &state == state_ && gpu.cp[0x50] == preparedDescriptor_ && frame == preparedFrame_ &&
        gpu.bpChangesBesideColors == preparedBeside_ && gpu.xfRegisterChanges == preparedXfRegisters_) {
        // Only TEV colors or XF matrix memory changed (a particle's): C0's bytes, the folds and the clip matrices again.
        if (gpu.bpChanges != preparedBp_) {
            if (color0InVertex_ && !gxColor0Bytes(gpu, color0_)) {
                unsupported = "C0 outside a byte while it rides in the vertex color";
                return prepared_ = false;
            }
            preparedBp_ = gpu.bpChanges;
        }
        if (gpu.xfChanges != preparedXf_) {
            for (unsigned u = 0; u < units_; ++u)
                if (!texgens_[u].palette && !foldHolds(texgens_[u])) refold(texgens_[u]);
            if (channel_ && lightingLights_ != gpu.xfLightChanges) readLighting(gpu);
            preparedXf_ = xfSeen_ = gpu.xfChanges, clipReady_[0] = clipReady_[1] = 0;
        }
        kind_ = kind, preparedFormat_ = format;
        return true;
    }
    prepared_ = false, ++fullPrepares;
    readPlans_[0].layout = readPlans_[1].layout = nullptr;
    if (foldsFor_ != &gpu) {
        for (Fold& f : folds_) f = Fold{};
        for (TexGen& t : texgens_) t.foldOf = nullptr;
        foldsFor_ = &gpu;
    }
    gpu_ = &gpu, state_ = &state, kind_ = kind;
    unsupported = state.unsupported;
    if (unsupported) return false;
    // GEN_MODE's reject bits as GXSetCullMode swaps them in: 1 culls what GX_CULL_BACK names, 2 GX_CULL_FRONT.
    reject_ = static_cast<std::uint8_t>(GEN_MODE_GET_REJECT_EN(gpu.bp[0x00]));
    perVertexMatrix_ = gpu.cp[0x50] & 1;
    defaultMatrix_ = gpu.xf[XfMatIndexA] & 0x3F;
    channel_ = (gpu.xf[XfNumChans] & 3) != 0;
    color0InVertex_ = state.color0InVertex;
    if (color0InVertex_ && !gxColor0Bytes(gpu, color0_)) {
        unsupported = "C0 outside a byte while it rides in the vertex color";
        return false;
    }
    lit_ = channel_ && (XF_COLOR0CNTRL_F_GET_LIGHTFUNC(gpu.xf[XfColor0Ctrl]) || XF_COLOR0CNTRL_F_GET_LIGHTFUNC(gpu.xf[XfAlpha0Ctrl]));
    fixedDepth_ = state.fixedDepth, depth01_ = state.depth01;
    units_ = static_cast<std::uint8_t>(state.program.unitCount);
    for (unsigned u = 0; u < units_; ++u) {
        TexGen& t = texgens_[u];
        t.palette = state.program.units[u].texture == CombinerProgram::Palette;
        if (t.palette) {
            t.kind = TexGen::Palette;
            continue;
        }
        const std::uint8_t coord = state.program.units[u].texCoord;
        const std::uint32_t ctrl = gpu.xf[XfTexGen + coord];
        const std::uint32_t row = XF_TEX_GET_SOURCE_ROW(ctrl);
        if (XF_TEX_GET_TEXGEN_TYPE(ctrl) != 0) unsupported = "emboss and color texgens";
        else if (row != 0 && row != 1 && (row < 5 || row > 12)) unsupported = "texgens from colors or binormals";
        if (unsupported) return false;
        t.source = static_cast<std::uint8_t>(row), t.coord = coord;
        t.threeInputs = XF_TEX_GET_INPUT_FORM(ctrl);  // AB11 otherwise: the third input is one whatever the source
        t.rows = XF_TEX_GET_PROJECTION(ctrl) ? 3 : 2;
        t.perVertexMatrix = gpu.cp[0x50] >> (1 + coord) & 1;
        t.matrix = coord < 4 ? gpu.xf[XfMatIndexA] >> (6 + 6 * coord) & 0x3F : gpu.xf[XfMatIndexB] >> (6 * (coord - 4)) & 0x3F;
        t.dual = gpu.xf[XfDualTex] & 1;
        const std::uint32_t post = gpu.xf[XfPostTexGen + coord];
        t.normalize = XF_DUALTEX_F_GET_NORMAL_ENABLE(post);
        t.post = XfPostMatrices / 4 + XF_DUALTEX_F_GET_DUALMATRIX_ADRS(post);
        t.ctrl = ctrl, t.postCtrl = post, t.foldOf = nullptr;
        t.folded = !t.perVertexMatrix && !(t.dual && t.normalize);
        t.input = static_cast<std::uint16_t>(row == 0 ? offsetof(GxVertex, position) : row == 1 ? offsetof(GxVertex, normal) : offsetof(GxVertex, texcoord) + 8 * (row - 5));
        refold(t);
    }
    // What a vertex is read for: the position, the texgens' sources, the normal when lit, the color when the channel takes it.
    sourceMask_ = 0;
    for (unsigned u = 0; u < units_; ++u) {
        const TexGen& t = texgens_[u];
        if (!t.palette) sourceMask_ = static_cast<std::uint16_t>(sourceMask_ | (t.source == 0 ? GxVertexLayout::ReadPosition : t.source == 1 ? GxVertexLayout::ReadNormal : 1u << (4 + t.source - 5)));
    }
    if (channel_ && (lightingRegisters_ != gpu.xfRegisterChanges || lightingLights_ != gpu.xfLightChanges)) readLighting(gpu);
    const GxLighting::Part* const parts = lighting_.parts;
    // The vertex's color, when the channel takes a material from it, or a lit part its ambient.
    const bool vertexColor = channel_ && !color0InVertex_ &&
                             (parts[0].materialFromVertex || parts[1].materialFromVertex ||
                              (lit_ && ((parts[0].lit && parts[0].ambientFromVertex) || (parts[1].lit && parts[1].ambientFromVertex))));
    readMask_ = static_cast<std::uint16_t>(GxVertexLayout::ReadPosition | sourceMask_ | (lit_ ? GxVertexLayout::ReadNormal : 0) |
                                           (vertexColor ? GxVertexLayout::ReadColor0 : 0));
    prepared_ = true, preparedFormat_ = format, preparedFrame_ = frame, preparedDescriptor_ = gpu.cp[0x50];
    preparedBp_ = gpu.bpChanges, preparedXf_ = gpu.xfChanges;
    preparedBeside_ = gpu.bpChangesBesideColors, preparedXfRegisters_ = gpu.xfRegisterChanges;
    if (gpu.xfChanges == xfSeen_ && frame == frameSeen_) return true;
    xfSeen_ = gpu.xfChanges, clipReady_[0] = clipReady_[1] = 0;
    // Most XF loads are matrices; the projection and viewport change far less often.
    if (!(frame == frameSeen_) || !sameWords(gpu.xf + XfViewport, viewSeen_, 13)) {
        frameSeen_ = frame, ++toClipEpoch_;
        std::memcpy(viewSeen_, gpu.xf + XfViewport, sizeof viewSeen_);
        // View space (x, y, z, 1) to clip space, then the viewport onto the EFB, then the EFB onto [-1, 1] (y up) with
        // depth onto citro3d's [-1, 0]: EFB pixel = ndc * scale + offset - 342; depth = ndc_z * zscale + zoffset, over 2^24 - 1.
        double p[6], vp[6];
        for (int k = 0; k < 6; ++k) p[k] = asFloat(gpu.xf[XfProjection + k]), vp[k] = asFloat(gpu.xf[XfViewport + k]);
        const bool orthographic = gpu.xf[XfProjection + 6] != 0;
        const double X[4] = {p[0], 0, orthographic ? 0 : p[1], orthographic ? p[1] : 0};
        const double Y[4] = {0, p[2], orthographic ? 0 : p[3], orthographic ? p[3] : 0};
        const double Z[4] = {0, 0, p[4], p[5]};
        const double W[4] = {0, 0, orthographic ? 0.0 : -1.0, orthographic ? 1.0 : 0.0};
        double d[4][4];
        for (int c = 0; c < 4; ++c) {
            const double ex = X[c] * vp[0] + (vp[3] - 342) * W[c], ey = Y[c] * vp[1] + (vp[4] - 342) * W[c];
            d[0][c] = ex / frame.width * 2 - W[c];
            d[1][c] = W[c] - ey / frame.height * 2;
            d[2][c] = (Z[c] * vp[2] + vp[5] * W[c]) / 16777215.0 - W[c];
            d[3][c] = W[c];
        }
        for (int c = 0; c < 4; ++c) {
            toClip_[0][c] = frame.quarterTurn ? d[1][c] : d[0][c];
            toClip_[1][c] = frame.quarterTurn ? -d[0][c] : d[1][c];
            toClip_[2][c] = d[2][c], toClip_[3][c] = d[3][c];
            for (int r = 0; r < 4; ++r) toClipFloat_[r][c] = static_cast<float>(toClip_[r][c]);
        }
    }
    return true;
}

// A primitive's texgen usually reuses the last one's matrices: their words are compared rather than folded again.
bool GxVertexPipeline::foldStillHolds(Fold& f, const TexGen& t, std::uint32_t ctrl, std::uint32_t post) const {
    if (f.foldXf == gpu_->xfChanges && f.sourceWords && f.source_[0] == t.coord && f.source_[1] == ctrl) return true;
    // A load that left the fold's own setup and rows as they were (most often a particle's position matrix).
    const std::uint32_t* rows = gpu_->xfRowChanges;
    bool same = f.sourceWords == 5u + 4u * t.rows + (t.dual ? 12u : 0u) && f.source_[0] == t.coord && f.source_[1] == ctrl &&
                f.source_[2] == t.dual && f.source_[3] == post && f.source_[4] == t.matrix;
    for (int r = 0; r < t.rows && same; ++r) same = rows[(t.matrix + r) & 0x3FF] == f.rowChanges[r];
    for (int r = 0; r < 3 && same && t.dual; ++r) same = rows[(t.post + r) & 0x3FF] == f.rowChanges[3 + r];
    if (same) return f.foldXf = gpu_->xfChanges, true;
    std::uint32_t words[32];
    std::uint32_t n = 0;
    words[n++] = t.coord, words[n++] = ctrl, words[n++] = t.dual, words[n++] = post, words[n++] = t.matrix;
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < 4; ++c) words[n++] = gpu_->xf[((t.matrix + r) * 4 + c) & 0xFFF];
    if (t.dual)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) words[n++] = gpu_->xf[((t.post + r) * 4 + c) & 0xFFF];
    same = n == f.sourceWords && sameWords(words, f.source_, n);
    for (std::uint32_t i = 0; i < n; ++i) f.source_[i] = words[i];
    f.sourceWords = n, f.foldXf = gpu_->xfChanges;
    for (int r = 0; r < t.rows; ++r) f.rowChanges[r] = rows[(t.matrix + r) & 0x3FF];
    for (int r = 0; r < 3 && t.dual; ++r) f.rowChanges[3 + r] = rows[(t.post + r) & 0x3FF];
    return same;
}

// refold's early return without its calls: a folded texgen whose matrix rows (and dual rows) are as its fold was made from. Only
// after a full prepare, which left the fold made from this texgen's setup.
bool GxVertexPipeline::foldHolds(const TexGen& t) const {
    if (!t.folded) return false;
    Fold& f = folds_[t.coord & 7];
    if (t.foldOf != &f || t.foldAt != f.version) return false;
    const std::uint32_t* rows = gpu_->xfRowChanges;
    for (int r = 0; r < t.rows; ++r)
        if (rows[(t.matrix + r) & 0x3FF] != f.rowChanges[r]) return false;
    for (int r = 0; r < 3 && t.dual; ++r)
        if (rows[(t.post + r) & 0x3FF] != f.rowChanges[3 + r]) return false;
    f.foldXf = gpu_->xfChanges;
    return true;
}

// A folded texgen's map again when the matrices it was made from changed, and whether it is planar.
void GxVertexPipeline::refold(TexGen& t) const {
    Fold& f = folds_[t.coord & 7];
    if (t.folded) {
        if (!foldStillHolds(f, t, t.ctrl, t.postCtrl)) {
            ++foldsMade;
            // The matrix's rows (a third of (0, 0, 0, 1) without projection), then the dual transform over them.
            double m[3][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1}};
            for (int r = 0; r < t.rows; ++r)
                for (int c = 0; c < 4; ++c) m[r][c] = matrix(t.matrix + r, c);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 4; ++c) {
                    double sum = m[r][c];
                    if (t.dual) {
                        sum = c == 3 ? matrix(t.post + r, 3) : 0;
                        for (int k = 0; k < 3; ++k) sum += matrix(t.post + r, k) * m[k][c];
                    }
                    f.fold[r][c] = static_cast<float>(sum);
                }
            ++f.version;
        }
        if (t.foldOf == &f && t.foldAt == f.version) return;  // the texgen has this fold already
        std::memcpy(t.fold, f.fold, sizeof t.fold);
        t.foldOf = &f, t.foldAt = f.version;
    }
    // A texcoord source, or the AB11 form, gives a third input of one; with q fixed at one there's nothing to divide.
    const bool thirdIsOne = t.source >= 5 || !t.threeInputs;
    const bool planar = t.folded && thirdIsOne && t.fold[2][0] == 0 && t.fold[2][1] == 0 && t.fold[2][2] == 0 && t.fold[2][3] == 1;
    // A third row of (0, 0, 0, 1), or (0, 0, 1, 0) over a third input of one, leaves q at one (NaN from an infinite input): never divided.
    const bool flat = t.folded && t.fold[2][0] == 0 && t.fold[2][1] == 0 &&
                      ((t.fold[2][2] == 0 && t.fold[2][3] == 1) || (thirdIsOne && t.fold[2][2] == 1 && t.fold[2][3] == 0));
    t.cached = !t.folded && !t.perVertexMatrix;
    t.kind = t.cached ? TexGen::Normalized : !t.folded ? TexGen::Unfolded : planar ? TexGen::Planar : flat ? TexGen::Affine : TexGen::Folded;
    if (t.cached && f.matricesXf != gpu_->xfChanges) loadMatrices(f, t);
}

// A normalized texgen's two matrices as floats.
void GxVertexPipeline::loadMatrices(Fold& f, const TexGen& t) const {
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < 4; ++c) f.matrix_[r][c] = matrix(t.matrix + r, c);
    f.postIdentity = true;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            f.post_[r][c] = matrix(t.post + r, c);
            std::uint32_t bits;
            std::memcpy(&bits, &f.post_[r][c], 4);
            f.postIdentity &= bits == (r == c ? 0x3F800000u : 0u);
        }
    f.matricesXf = gpu_->xfChanges;
}

// A position matrix (three rows from `index`) through toClip_: one 4x4 per vertex instead of a matrix, the projection
// and the viewport.
const float* GxVertexPipeline::makeClipMatrix(std::uint32_t index) {
    float* out = index < 64 ? clip_[index] : clipScratch_;
    // The matrix's words where they lie (a copy only where the index wraps), and toClip_ in locals: each store to `out` would
    // make the compiler read what it can't prove untouched again.
    std::uint32_t wrapped[12];
    const std::uint32_t first = index * 4 & 0xFFF;
    const std::uint32_t* words = gpu_->xf + first;
    if (first + 12 > 0x1000) {
        for (unsigned i = 0; i < 12; ++i) wrapped[i] = gpu_->xf[(index * 4 + i) & 0xFFF];
        words = wrapped;
    }
    float t[16];
    for (int i = 0; i < 16; ++i) t[i] = toClipFloat_[i >> 2][i & 3];
    for (int c = 0; c < 4; ++c) {
        const float m0 = asFloat(words[c]), m1 = asFloat(words[4 + c]), m2 = asFloat(words[8 + c]);
        for (int r = 0; r < 4; ++r) {
            float sum = c == 3 ? t[r * 4 + 3] : 0;  // the position matrix's fourth row is (0, 0, 0, 1)
            sum += t[r * 4] * m0, sum += t[r * 4 + 1] * m1, sum += t[r * 4 + 2] * m2;
            out[r * 4 + c] = sum;
        }
    }
    if (index < 64) clipReady_[index >> 5] |= 1u << (index & 31);
    return out;
}

// One texture coordinate set, for a texgen that takes its matrix per vertex or normalizes (texcoords() does the others): the
// texgen's input through its matrix, then the dual transform when it is on.
void GxVertexPipeline::texgen(const TexGen& t, const GxVertex& v, float st[2]) const {
    if (t.cached) return texgenNormalized(t, v, st);
    float c[3];
    float in[4] = {0, 0, 1, 1};
    if (t.source == 0) std::memcpy(in, v.position, sizeof v.position);
    else if (t.source == 1) std::memcpy(in, v.normal, sizeof v.normal);
    else in[0] = v.texcoord[t.source - 5][0], in[1] = v.texcoord[t.source - 5][1];
    if (!t.threeInputs) in[2] = 1;
    const std::uint32_t index = t.perVertexMatrix ? v.texMatrixIndex[t.coord] : t.matrix;
    if (t.perVertexMatrix) {
        ++(index == v.matrixIndex ? texMatrixShared : index == v.matrixIndex + 30u ? texMatrixPaired : texMatrixOwn);
        texMatrixRepeated += index == lastTexMatrix, lastTexMatrix = index;
    }
    texgensNormalized += t.dual && t.normalize;
    c[0] = 0, c[1] = 0, c[2] = 1;
    for (int r = 0; r < t.rows; ++r)
        c[r] = matrix(index + r, 0) * in[0] + matrix(index + r, 1) * in[1] + matrix(index + r, 2) * in[2] + matrix(index + r, 3) * in[3];
    if (t.dual) {
        if (t.normalize) {
            const float length = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            if (length > 0) for (float& k : c) k /= length;
        }
        float q[3];
        for (int r = 0; r < 3; ++r) q[r] = matrix(t.post + r, 0) * c[0] + matrix(t.post + r, 1) * c[1] + matrix(t.post + r, 2) * c[2] + matrix(t.post + r, 3);
        std::memcpy(c, q, sizeof c);
    }
    // The GPU divides by q for each pixel; here it is divided once per vertex.
    if (std::fabs(c[2] - 1) > 1e-5f && c[2] != 0) st[0] = c[0] / c[2], st[1] = c[1] / c[2];
    else st[0] = c[0], st[1] = c[1];
}

// texgen() for a normalized texgen, the matrices read as floats. An identity post matrix only adds zeros: skipped, which
// gives the same bits for finite input.
void GxVertexPipeline::texgenNormalized(const TexGen& t, const GxVertex& v, float st[2]) const {
    const Fold& f = folds_[t.coord & 7];
    const float* in = reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(&v) + t.input);
    const float z = t.threeInputs && t.source <= 1 ? in[2] : 1.0f;
    const float (*m)[4] = f.matrix_;
    float c0 = m[0][0] * in[0] + m[0][1] * in[1] + m[0][2] * z + m[0][3];
    float c1 = m[1][0] * in[0] + m[1][1] * in[1] + m[1][2] * z + m[1][3];
    float c2 = t.rows == 3 ? m[2][0] * in[0] + m[2][1] * in[1] + m[2][2] * z + m[2][3] : 1.0f;
    ++texgensNormalized;
    const float length = std::sqrt(c0 * c0 + c1 * c1 + c2 * c2);
    if (length > 0) c0 /= length, c1 /= length, c2 /= length;
    float q0, q1, q2;
    if (f.postIdentity) {
        q0 = c0 + 0.0f, q1 = c1 + 0.0f, q2 = c2 + 0.0f;
    } else {
        const float (*p)[4] = f.post_;
        q0 = p[0][0] * c0 + p[0][1] * c1 + p[0][2] * c2 + p[0][3];
        q1 = p[1][0] * c0 + p[1][1] * c1 + p[1][2] * c2 + p[1][3];
        q2 = p[2][0] * c0 + p[2][1] * c1 + p[2][2] * c2 + p[2][3];
    }
    if (std::fabs(q2 - 1) > 1e-5f && q2 != 0) st[0] = q0 / q2, st[1] = q1 / q2;
    else st[0] = q0, st[1] = q1;
}

const GxVertexPipeline::ReadPlan& GxVertexPipeline::readPlan(unsigned slot, const GxVertexLayout& layout, std::uint16_t mask) {
    ReadPlan& p = readPlans_[slot];
    if (p.layout == &layout && p.gpu == gpu_ && p.cpChanges == gpu_->cpChanges && p.mask == mask) return p;
    p.gpu = gpu_, p.layout = &layout, p.cpChanges = gpu_->cpChanges, p.mask = mask, p.zeros = 0;
    p.plan = layout.plan(mask);
    // Per attribute (the mask's bits): its components (a color counts as one) and where it starts in a GxVertex.
    constexpr std::uint8_t Components[12] = {3, 3, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2};
    for (unsigned a = 0; a < 12; ++a) {
        if (!(mask >> a & 1) || p.plan.width[a] >= Components[a]) continue;
        const std::size_t start = a == 0   ? offsetof(GxVertex, position)
                                  : a == 1 ? offsetof(GxVertex, normal)
                                  : a < 4  ? offsetof(GxVertex, color) + 4 * (a - 2)
                                           : offsetof(GxVertex, texcoord) + 8 * (a - 4);
        p.zero[p.zeros++] = {static_cast<std::uint16_t>(start + 4 * p.plan.width[a]), static_cast<std::uint8_t>(4 * (Components[a] - p.plan.width[a]))};
    }
    return p;
}

std::size_t GxVertexPipeline::run(const std::uint8_t* data, std::uint16_t count, GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices) {
    const GxVertexLayout& layout = gpu_->layout(preparedFormat_);  // a mesh's draw never needs it, so prepare leaves it
    const std::uint32_t size = layout.size;
    const ReadPlan& read = readPlan(0, layout, readMask_);
    const float* const fixedClip = perVertexMatrix_ ? nullptr : clipMatrix(defaultMatrix_);
    // The plan writes what it reads: only what it leaves out (an absent attribute) is zeroed, in the vertices there are.
    alignas(GxVertex) std::uint8_t storage[ReadChunk * sizeof(GxVertex)];
    GxVertex* const chunk = reinterpret_cast<GxVertex*>(storage);
    for (unsigned k = 0; k < read.zeros; ++k)
        for (unsigned j = 0; j < std::min<unsigned>(ReadChunk, count); ++j)
            std::memset(storage + j * sizeof(GxVertex) + read.zero[k].at, 0, read.zero[k].bytes);
    GxDrawVertex* const into = kind_ >= 0xA8 ? vertices + 3 * count : vertices;
    for (unsigned i = 0; i < count; i += ReadChunk) {
        const unsigned n = std::min<unsigned>(ReadChunk, count - i);
        layout.read(data + i * size, chunk, n, read.plan);
        this->vertices(chunk, n, into + i, fixedClip);
    }
    if (kind_ >= 0xA8) return widen(vertices, count, first, indices);
    verticesWritten = count;
    return triangles(vertices, count, first, indices);
}

std::size_t GxVertexPipeline::runDecoded(const GxVertex* decoded, std::uint16_t count, GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices) {
    GxDrawVertex* const into = kind_ >= 0xA8 ? vertices + 3 * count : vertices;
    const float* const fixedClip = perVertexMatrix_ ? nullptr : clipMatrix(defaultMatrix_);
    for (unsigned i = 0; i < count; i += ReadChunk) this->vertices(decoded + i, std::min<unsigned>(ReadChunk, count - i), into + i, fixedClip);
    if (kind_ >= 0xA8) return widen(vertices, count, first, indices);
    verticesWritten = count;
    return triangles(vertices, count, first, indices);
}

bool GxVertexPipeline::runPass(const std::uint8_t* data, const GxVertex* decoded, std::uint16_t count, const GxDrawVertex* program,
                                GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices, std::size_t& written) {
    if (kind_ >= 0xA8 || atlasUnit_ >= 0) return false;
    const GxVertexLayout& layout = gpu_->layout(preparedFormat_);
    const ReadPlan& read = readPlan(1, layout, sourceMask_);  // only what its own texgens read
    alignas(GxVertex) std::uint8_t storage[ReadChunk * sizeof(GxVertex)];
    GxVertex* const chunk = reinterpret_cast<GxVertex*>(storage);
    for (unsigned k = 0; k < read.zeros && !decoded; ++k)
        for (unsigned j = 0; j < std::min<unsigned>(ReadChunk, count); ++j)
            std::memset(storage + j * sizeof(GxVertex) + read.zero[k].at, 0, read.zero[k].bytes);
    for (unsigned i0 = 0; i0 < count; i0 += ReadChunk) {
        const unsigned n = std::min<unsigned>(ReadChunk, count - i0);
        if (!decoded) layout.read(data + i0 * layout.size, chunk, n, read.plan);
        for (unsigned j = 0; j < n; ++j) {
            GxDrawVertex& d = vertices[i0 + j];
            std::memcpy(d.position, program[i0 + j].position, sizeof d.position), std::memcpy(d.color, program[i0 + j].color, sizeof d.color);
        }
        texcoords(decoded ? decoded + i0 : chunk, n, vertices + i0);
    }
    verticesWritten = count;
    written = triangles(vertices, count, first, indices);
    return true;
}

std::size_t GxVertexPipeline::widen(GxDrawVertex* vertices, std::uint16_t count, std::uint16_t first, std::uint16_t* indices) {
    const GxDrawVertex* const from = vertices + 3 * count;
    const bool points = kind_ == 0xB8;
    // SU_LPSIZE: line width and point size in sixths of an EFB pixel, and the texture offset each spreads across itself
    // (GX_TO_ZERO to GX_TO_ONE); SU_SSIZE says per coordinate whether it takes the line's or the point's.
    const std::uint32_t sizes = gpu_->bp[0x22];
    const float half = static_cast<float>(points ? sizes >> 8 & 0xFF : sizes & 0xFF) / 12.0f;
    constexpr float Offsets[8] = {0, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1, 0, 0};
    const float offset = Offsets[points ? sizes >> 19 & 7 : sizes >> 16 & 7];
    float spread[CombinerProgram::MaxUnits][2] = {};
    for (unsigned u = 0; u < units_; ++u) {
        if (texgens_[u].palette || !(gpu_->bp[0x30 + 2 * texgens_[u].coord] >> (points ? 19 : 18) & 1)) continue;
        const bool atlas = static_cast<int>(u) == atlasUnit_;
        spread[u][0] = offset * (atlas ? atlasPlace_[0] : 1), spread[u][1] = offset * (atlas ? atlasPlace_[1] : 1);
    }
    // A move of (dx, dy) EFB pixels (y down) in clip space, as the frame lays the EFB out.
    const float toX = 2 / preparedFrame_.width, toY = 2 / preparedFrame_.height;
    const auto moved = [&](GxDrawVertex v, float dx, float dy, float ds, float dt, unsigned side) {
        const float w = v.position[3], nx = dx * toX * w, ny = -dy * toY * w;
        if (preparedFrame_.quarterTurn) v.position[1] -= nx, v.position[0] += ny;
        else v.position[0] += nx, v.position[1] += ny;
        for (unsigned u = 0; u < units_; ++u)
            v.texcoord[u][0] += spread[u][0] * ds * side, v.texcoord[u][1] += spread[u][1] * dt * side;
        if (atlasUnit_ >= 0 && side) {
            const float* st = v.texcoord[atlasUnit_];
            const float s = (st[0] - atlasPlace_[2]) / atlasPlace_[0], t = (st[1] - atlasPlace_[3]) / atlasPlace_[1];
            atlasOutside_ |= s < -1.0f / 4096 || s > 1 + 1.0f / 4096 || t < -1.0f / 4096 || t > 1 + 1.0f / 4096;
        }
        return v;
    };
    const auto efb = [&](const GxDrawVertex& v, float& x, float& y) {
        const float* p = v.position;
        const float ndcX = preparedFrame_.quarterTurn ? -p[1] / p[3] : p[0] / p[3], ndcY = preparedFrame_.quarterTurn ? p[0] / p[3] : p[1] / p[3];
        x = (ndcX + 1) / toX, y = (1 - ndcY) / toY;
    };
    std::size_t n = 0;
    std::uint16_t quads = 0;
    const auto quad = [&](const GxDrawVertex (&c)[4]) {
        for (int k = 0; k < 4; ++k) vertices[4 * quads + k] = c[k];
        const auto at = static_cast<std::uint16_t>(first + 4 * quads);
        for (int k : {0, 1, 2, 0, 2, 3}) indices[n++] = static_cast<std::uint16_t>(at + k);
        ++quads;
    };
    if (points) {
        for (std::uint16_t i = 0; i < count; ++i) {
            const GxDrawVertex v = from[i];
            if (v.position[3] <= 0) continue;
            quad({moved(v, -half, -half, 0, 0, 1), moved(v, half, -half, 1, 0, 1), moved(v, half, half, 1, 1, 1), moved(v, -half, half, 0, 1, 1)});
        }
    } else {
        // Widened across their minor axis, as GX rasterizes them; the texture offset goes to one side.
        for (std::uint16_t i = 0; i + 1 < count; i = static_cast<std::uint16_t>(i + (kind_ == 0xB0 ? 1 : 2))) {
            const GxDrawVertex a = from[i], b = from[i + 1];
            if (a.position[3] <= 0 || b.position[3] <= 0) continue;
            float ax, ay, bx, by;
            efb(a, ax, ay), efb(b, bx, by);
            const bool across = std::fabs(bx - ax) >= std::fabs(by - ay);
            const float dx = across ? 0 : half, dy = across ? half : 0;
            quad({moved(a, -dx, -dy, 0, 0, 0), moved(b, -dx, -dy, 0, 0, 0), moved(b, dx, dy, 1, 0, 1), moved(a, dx, dy, 1, 0, 1)});
        }
    }
    verticesWritten = static_cast<std::uint16_t>(4 * quads);
    return n;
}

void GxVertexPipeline::vertices(const GxVertex* v, unsigned n, GxDrawVertex* d, const float* fixedClip) {
    // The position through a clip matrix: a primitive's, or each vertex's own. The position is read first, as the stores may alias it.
    const auto transform = [](const float* m, const float* p, float* out) {
        const float x = p[0], y = p[1], z = p[2];
        for (int r = 0; r < 4; ++r) out[r] = m[r * 4] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3];
    };
    if (fixedClip) {
        const float* c = fixedClip;
        const float m[16] = {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]};
        for (unsigned j = 0; j < n; ++j) transform(m, v[j].position, d[j].position);
    } else {
        for (unsigned j = 0; j < n; ++j) transform(clipMatrix(v[j].matrixIndex), v[j].position, d[j].position);
    }
    if (fixedDepth_) {
        const float depth = depth01_ - 1;
        for (unsigned j = 0; j < n; ++j) d[j].position[2] = depth * d[j].position[3];
    }
    if (color0InVertex_) {
        std::uint32_t color;
        std::memcpy(&color, color0_, 4);
        for (unsigned j = 0; j < n; ++j) std::memcpy(d[j].color, &color, 4);
    } else if (!channel_) {
        for (unsigned j = 0; j < n; ++j) std::memset(d[j].color, 0, 4);
    } else if (!lit_) {
        // Nothing lit: each component is the vertex's color or the material register.
        const GxLighting::Part* parts = lighting_.parts;
        if (parts[0].materialFromVertex && parts[1].materialFromVertex) {
            for (unsigned j = 0; j < n; ++j) std::memcpy(d[j].color, v[j].color[0], 4);
        } else if (!parts[0].materialFromVertex && !parts[1].materialFromVertex) {
            for (unsigned j = 0; j < n; ++j) std::memcpy(d[j].color, lighting_.material, 4);
        } else {
            for (unsigned j = 0; j < n; ++j)
                for (int c = 0; c < 4; ++c) d[j].color[c] = parts[c < 3 ? 0 : 1].materialFromVertex ? v[j].color[0][c] : lighting_.material[c];
        }
    } else {
        // The vertex's position matrix and its normal matrix, read again only when the vertex names another one.
        float modelView[12] = {}, normalMatrix[9] = {};
        std::uint32_t loaded = ~0u;
        for (unsigned j = 0; j < n; ++j) {
            const std::uint32_t index = fixedClip ? defaultMatrix_ : v[j].matrixIndex;
            if (index != loaded) {
                copyRows(modelView, gpu_->xf, index, 3);
                // Normal matrices: three rows of three from XF 0x400, at the position matrix's row.
                const std::uint32_t base = 0x400 + (index & 31) * 3;
                for (int k = 0; k < 9; ++k) normalMatrix[k] = asFloat(gpu_->xf[base + k]);
                loaded = index;
            }
            const float x = v[j].position[0], y = v[j].position[1], z = v[j].position[2];
            const float nx = v[j].normal[0], ny = v[j].normal[1], nz = v[j].normal[2];
            float view[3], normal[3];
            for (int r = 0; r < 3; ++r) view[r] = modelView[r * 4] * x + modelView[r * 4 + 1] * y + modelView[r * 4 + 2] * z + modelView[r * 4 + 3];
            for (int r = 0; r < 3; ++r) normal[r] = normalMatrix[r * 3] * nx + normalMatrix[r * 3 + 1] * ny + normalMatrix[r * 3 + 2] * nz;
            normalize(normal);
            lighting_.apply(view, normal, v[j].color[0], d[j].color);
        }
    }
    texcoords(v, n, d);
    if (atlasUnit_ >= 0) {
        constexpr float slack = 1.0f / 4096;
        for (unsigned j = 0; j < n; ++j) {
            float* st = d[j].texcoord[atlasUnit_];
            atlasOutside_ |= st[0] < -slack || st[0] > 1 + slack || st[1] < -slack || st[1] > 1 + slack;
            st[0] = st[0] * atlasPlace_[0] + atlasPlace_[2], st[1] = st[1] * atlasPlace_[1] + atlasPlace_[3];
        }
    }
}

void GxVertexPipeline::texcoords(const GxVertex* v, unsigned n, GxDrawVertex* d) const {
    bool perVertex = false;
    for (unsigned u = 0; u < units_; ++u) {
        const TexGen& t = texgens_[u];
        const auto input = [&](unsigned j) { return reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(&v[j]) + t.input); };
        switch (t.kind) {
        case TexGen::Palette: {
            const float s = palette_[0], r = palette_[1];  // locals: each store to a vertex would make the compiler read them again
            for (unsigned j = 0; j < n; ++j) d[j].texcoord[u][0] = s, d[j].texcoord[u][1] = r;
            break;
        }
        case TexGen::Planar: {
            const float a0 = t.fold[0][0], b0 = t.fold[0][1], k0 = t.fold[0][2] + t.fold[0][3];
            const float a1 = t.fold[1][0], b1 = t.fold[1][1], k1 = t.fold[1][2] + t.fold[1][3];
            for (unsigned j = 0; j < n; ++j) {
                const float s = input(j)[0], r = input(j)[1];
                d[j].texcoord[u][0] = a0 * s + b0 * r + k0;
                d[j].texcoord[u][1] = a1 * s + b1 * r + k1;
            }
            break;
        }
        case TexGen::Affine: case TexGen::Folded: {
            const bool third = t.threeInputs && t.source <= 1;  // else the third input is one
            const float f00 = t.fold[0][0], f01 = t.fold[0][1], f02 = t.fold[0][2], f03 = t.fold[0][3];
            const float f10 = t.fold[1][0], f11 = t.fold[1][1], f12 = t.fold[1][2], f13 = t.fold[1][3];
            const float f20 = t.fold[2][0], f21 = t.fold[2][1], f22 = t.fold[2][2], f23 = t.fold[2][3];
            const bool divides = t.kind == TexGen::Folded;
            for (unsigned j = 0; j < n; ++j) {
                const float* in = input(j);
                const float s = in[0], r = in[1], z = third ? in[2] : 1.0f;
                const float c0 = f00 * s + f01 * r + f02 * z + f03, c1 = f10 * s + f11 * r + f12 * z + f13;
                if (divides) {
                    const float c2 = f20 * s + f21 * r + f22 * z + f23;
                    if (std::fabs(c2 - 1) > 1e-5f && c2 != 0) d[j].texcoord[u][0] = c0 / c2, d[j].texcoord[u][1] = c1 / c2;
                    else d[j].texcoord[u][0] = c0, d[j].texcoord[u][1] = c1;
                } else {
                    d[j].texcoord[u][0] = c0, d[j].texcoord[u][1] = c1;
                }
            }
            break;
        }
        default:
            perVertex = true;
        }
    }
    // Texgens that take a matrix per vertex (or normalize) count what they do in vertex order, so they run in it.
    if (!perVertex) return;
    for (unsigned j = 0; j < n; ++j)
        for (unsigned u = 0; u < units_; ++u)
            if (texgens_[u].kind == TexGen::Unfolded || texgens_[u].kind == TexGen::Normalized) texgen(texgens_[u], v[j], d[j].texcoord[u]);
}

namespace {
bool behindNear(const float* p) { return p[2] < -p[3]; }

// The near plane (z = -w), then the guard band's four (GuardBand * w + sign * (x or y)), each kept where its distance
// isn't negative.
float clipDistance(const float* p, int plane) {
    if (plane == 4) return p[2] + p[3];
    return GxVertexPipeline::GuardBand * p[3] + (plane & 1 ? p[plane >> 1] : -p[plane >> 1]);
}

GxDrawVertex between(const GxDrawVertex& a, const GxDrawVertex& b, float t) {
    GxDrawVertex v;
    for (int c = 0; c < 4; ++c) v.position[c] = a.position[c] + (b.position[c] - a.position[c]) * t;
    for (int c = 0; c < 4; ++c) v.color[c] = static_cast<std::uint8_t>(std::lround(a.color[c] + (int(b.color[c]) - int(a.color[c])) * t));
    for (unsigned u = 0; u < CombinerProgram::MaxUnits; ++u)
        for (int c = 0; c < 2; ++c) v.texcoord[u][c] = a.texcoord[u][c] + (b.texcoord[u][c] - a.texcoord[u][c]) * t;
    return v;
}

// A triangle cut at the near plane and to the guard band (Sutherland-Hodgman, in clip space, so the PICA's
// perspective-correct interpolation of each attribute is unchanged): up to eight corners, a convex polygon.
unsigned clipToGuardBand(const GxDrawVertex& a, const GxDrawVertex& b, const GxDrawVertex& c, GxDrawVertex (&out)[8]) {
    GxDrawVertex buffers[2][8] = {{a, b, c}};
    unsigned n = 3, k = 0;
    for (int plane : {4, 0, 1, 2, 3}) {
        const GxDrawVertex* from = buffers[k & 1];
        GxDrawVertex* to = buffers[++k & 1];
        unsigned m = 0;
        for (unsigned i = 0; i < n; ++i) {
            const GxDrawVertex& p = from[i], &q = from[(i + 1) % n];
            const float dp = clipDistance(p.position, plane), dq = clipDistance(q.position, plane);
            if (dp >= 0) to[m++] = p;
            if ((dp >= 0) != (dq >= 0)) to[m++] = between(p, q, dp / (dp - dq));
        }
        if ((n = m) < 3) return 0;
    }
    std::copy(buffers[k & 1], buffers[k & 1] + n, out);
    return n;
}
} // namespace

std::size_t GxVertexPipeline::triangles(GxDrawVertex* vertices, std::uint16_t count, std::uint16_t first, std::uint16_t* indices) {
    // Culling in EFB space, by the sign of each triangle's area there: with every w positive that is the sign of the
    // determinant of its (x, y, w) rows, negated (the EFB's y runs down); the screen's quarter turn keeps it.
    std::size_t n = 0;
    const auto culled = [&](const float* pa, const float* pb, const float* pc) {
        if (!reject_ || pa[3] <= 0 || pb[3] <= 0 || pc[3] <= 0) return false;
        const float det = pa[0] * (pb[1] * pc[3] - pc[1] * pb[3]) - pb[0] * (pa[1] * pc[3] - pc[1] * pa[3]) + pc[0] * (pa[1] * pb[3] - pb[1] * pa[3]);
        return reject_ == 3 || (reject_ == 1 && det > 0) || (reject_ == 2 && det < 0);
    };
    // Only a triangle the PICA would cut at the near plane is cut here: its new corners are what land far off the screen.
    bool outside = false;
    for (std::uint16_t i = 0; i < count && !outside; ++i) outside = behindNear(vertices[i].position);
    const std::size_t room = maxVertices(kind_, count);
    std::size_t corners = count;  // where a cut triangle's corners go
    const auto emit = [&](int a, int b, int c) {
        const float* pa = vertices[a].position, *pb = vertices[b].position, *pc = vertices[c].position;
        if (outside && (behindNear(pa) || behindNear(pb) || behindNear(pc)) && corners + 8 <= room) {
            GxDrawVertex cut[8];
            const unsigned m = clipToGuardBand(vertices[a], vertices[b], vertices[c], cut);
            ++trianglesCut;
            const std::size_t at = corners;
            for (unsigned k = 0; k < m; ++k) vertices[corners++] = cut[k];
            for (unsigned k = 1; k + 1 < m; ++k) {
                if (culled(cut[0].position, cut[k].position, cut[k + 1].position)) continue;
                indices[n++] = static_cast<std::uint16_t>(first + at), indices[n++] = static_cast<std::uint16_t>(first + at + k);
                indices[n++] = static_cast<std::uint16_t>(first + at + k + 1);
            }
            return;
        }
        if (culled(pa, pb, pc)) return;
        indices[n++] = static_cast<std::uint16_t>(first + a), indices[n++] = static_cast<std::uint16_t>(first + b), indices[n++] = static_cast<std::uint16_t>(first + c);
    };
    if (!outside && !reject_ && kind_ == 0x98) {  // a strip with nothing culled, two triangles a step
        std::uint16_t* out = indices;
        std::uint16_t a = first;
        int i = 0;
        for (; i + 3 < count; i += 2, a = static_cast<std::uint16_t>(a + 2)) {
            out[0] = a, out[1] = static_cast<std::uint16_t>(a + 1), out[2] = static_cast<std::uint16_t>(a + 2);
            out[3] = static_cast<std::uint16_t>(a + 2), out[4] = static_cast<std::uint16_t>(a + 1), out[5] = static_cast<std::uint16_t>(a + 3);
            out += 6;
        }
        if (i + 2 < count) out[0] = a, out[1] = static_cast<std::uint16_t>(a + 1), out[2] = static_cast<std::uint16_t>(a + 2), out += 3;
        return static_cast<std::size_t>(out - indices);
    }
    if (!outside && !reject_ && (kind_ == 0x80 || kind_ == 0x88)) {  // quads with nothing culled, two triangles each
        std::uint16_t* out = indices;
        for (int i = 0; i + 3 < count; i += 4, out += 6) {
            out[0] = out[3] = static_cast<std::uint16_t>(first + i), out[1] = static_cast<std::uint16_t>(first + i + 1);
            out[2] = out[4] = static_cast<std::uint16_t>(first + i + 2), out[5] = static_cast<std::uint16_t>(first + i + 3);
        }
        return static_cast<std::size_t>(out - indices);
    }
    switch (kind_) {
    case 0x80: case 0x88:  // quads
        for (int i = 0; i + 3 < count; i += 4) emit(i, i + 1, i + 2), emit(i, i + 2, i + 3);
        break;
    case 0x90:  // triangles
        for (int i = 0; i + 2 < count; i += 3) emit(i, i + 1, i + 2);
        break;
    case 0x98:  // a strip: every other triangle turns the other way
        for (int i = 0; i + 2 < count; ++i) i & 1 ? emit(i + 1, i, i + 2) : emit(i, i + 1, i + 2);
        break;
    case 0xA0:  // a fan
        for (int i = 1; i + 1 < count; ++i) emit(0, i, i + 1);
        break;
    }
    verticesWritten = static_cast<std::uint16_t>(corners);
    return n;
}

namespace {
// The shader's light entries: a light under one part's settings, feeding the color, the alpha or both (both only when
// the two parts treat it alike). More than four can't be drawn (returns five).
struct LightEntry {
    std::uint8_t light, part;
    bool rgb, alpha;
};
unsigned lightEntries(const GxLighting& lighting, LightEntry out[4]) {
    unsigned count = 0;
    for (int i = 0; i < 2; ++i) {
        const GxLighting::Part& part = lighting.parts[i];
        if (!part.lit) continue;
        for (int n = 0; n < 8; ++n) {
            if (!(part.mask >> n & 1)) continue;
            if (i == 1 && lighting.parts[0].lit && lighting.parts[0].mask >> n & 1) {
                const GxLighting::Part& color = lighting.parts[0];
                if (color.diffuse == part.diffuse && color.attenuated == part.attenuated && color.spot == part.spot) {
                    for (unsigned e = 0; e < count; ++e)
                        if (out[e].light == n) out[e].alpha = true;
                    continue;
                }
            }
            if (count == 4) return 5;
            out[count++] = {static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(i), i == 0, i == 1};
        }
    }
    return count;
}
} // namespace

void GxVertexPipeline::packLights() {
    LightEntry entries[4];
    lightCount_ = lightEntries(lighting_, entries);
    const unsigned count = std::min(lightCount_, 4u);
    for (unsigned e = 0; e < count; ++e) {
        const GxLighting::Part& part = lighting_.parts[entries[e].part];
        const GxLighting::Light& l = lighting_.lights[entries[e].light];
        float (&packed)[6][4] = lightRows_[e];
        float cosine[3] = {l.cosine[0], l.cosine[1], l.cosine[2]}, distance[3] = {l.distance[0], l.distance[1], l.distance[2]};
        if (!part.attenuated) cosine[0] = distance[0] = 1, cosine[1] = cosine[2] = distance[1] = distance[2] = 0;  // a scale of one
        else if (!part.spot && part.diffuse) normalize(distance);  // as GxLighting's specular branch does
        const float far = std::max({std::fabs(l.position[0]), std::fabs(l.position[1]), std::fabs(l.position[2])});
        const float prescale = far > 1e6f ? 1 / far : 1;
        for (int c = 0; c < 4; ++c) packed[0][c] = (c < 3 ? entries[e].rgb : entries[e].alpha) ? l.color[c] / 255.0f : 0;
        for (int c = 0; c < 3; ++c) packed[1][c] = l.position[c] * prescale;
        packed[1][3] = cosine[0];
        packed[2][0] = l.direction[0], packed[2][1] = l.direction[1], packed[2][2] = l.direction[2], packed[2][3] = cosine[1];
        packed[3][0] = cosine[2], packed[3][1] = distance[0], packed[3][2] = distance[1], packed[3][3] = distance[2];
        packed[4][0] = -prescale, packed[4][1] = 1 / prescale, packed[4][2] = packed[4][3] = 0;
        packed[5][0] = part.attenuated && part.spot, packed[5][1] = part.diffuse != 0, packed[5][2] = part.diffuse == 2 ? 0.0f : -2.0f;
        packed[5][3] = 0;
    }
    for (int c = 0; c < 4; ++c) ambientRow_[c] = lighting_.ambient[c] / 255.0f;
}

bool GxVertexPipeline::meshColorFromVertex() const {
    const GxLighting::Part* parts = lighting_.parts;
    return channel_ && !color0InVertex_ &&
           (parts[0].materialFromVertex || parts[1].materialFromVertex || (lit_ && (parts[0].ambientFromVertex || parts[1].ambientFromVertex)));
}

unsigned GxVertexPipeline::meshEligible() const {
    if (unsupported) return MeshUnsupported;
    for (const GxDrawState::Pass& pass : state_->passes)
        if (!pass.sameUnits) return MeshPasses;
    for (unsigned u = 0; u < units_; ++u)
        if (texgens_[u].palette) return MeshUnsupported;  // the palette's texel moves per primitive
    if (fixedDepth_) return MeshDepthTexture;
    if (lit_) {
        // The shader runs up to four light entries, each a light under one part's settings (an actor lights its color
        // with lights 0 and 1, its alpha with light 2 and other settings).
        if (color0InVertex_) return MeshLitColor0;
        if (lightCount_ > 4) return MeshLights;
    }
    for (unsigned u = 0; u < units_; ++u)
        if (texgens_[u].perVertexMatrix && !(lit_ && perVertexMatrix_)) return MeshTexgen;
    for (unsigned u = 0; u < units_; ++u)
        if (texgens_[u].dual && texgens_[u].normalize) return MeshNormalize;
    return MeshOk;
}

bool GxVertexPipeline::meshSpan(const std::uint8_t* list, std::uint32_t size, const std::uint8_t*& lo, const std::uint8_t*& hi) const {
    lo = reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX), hi = nullptr;
    for (std::uint32_t at = 0; at < size;) {
        const std::uint8_t op = list[at];
        if (op == 0x00) {
            ++at;
            continue;
        }
        if (op < 0x80 || op >= 0xA8 || at + 3 > size) return false;
        const std::uint16_t count = static_cast<std::uint16_t>(list[at + 1] << 8 | list[at + 2]);
        const GxVertexLayout& layout = gpu_->layout(op & 7);
        if (at + 3 + std::uint32_t(count) * layout.size > size) return false;
        for (std::uint16_t i = 0; i < count; ++i) layout.indexedSpan(list + at + 3 + std::uint32_t(i) * layout.size, lo, hi);
        at += 3 + std::uint32_t(count) * layout.size;
    }
    return true;
}

unsigned GxVertexPipeline::buildMesh(const std::uint8_t* list, std::uint32_t size, GxMesh& mesh) const {
    mesh = GxMesh{};
    mesh.arraysLo = reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX), mesh.arraysHi = nullptr;
    mesh.layout = meshLayout();
    const GxMeshPacker packer(mesh.layout);
    const std::size_t stride = mesh.layout.stride;
    std::uint8_t slotTexgen[CombinerProgram::MaxUnits] = {};
    std::uint32_t at = 0;
    while (at < size) {
        const std::uint8_t op = list[at];
        if (op == 0x00) {
            ++at;
            continue;
        }
        if (op < 0x80 || op >= 0xA8 || at + 3 > size) return MeshCommand;  // state commands, lines and points stay the CPU's
        const std::uint8_t kind = op & 0xF8;
        const std::uint16_t count = static_cast<std::uint16_t>(list[at + 1] << 8 | list[at + 2]);
        const GxVertexLayout& layout = gpu_->layout(op & 7);
        mesh.formats = static_cast<std::uint8_t>(mesh.formats | 1u << (op & 7));
        if (at + 3 + std::uint32_t(count) * layout.size > size) return MeshCommand;
        const std::size_t first = mesh.vertexCount();
        if (first + count > 0x10000) return MeshTooLarge;
        mesh.vertices.resize((first + count) * stride);
        GxVertex v;  // attributes the format lacks stay zero for every vertex
        for (std::uint16_t i = 0; i < count; ++i) {
            layout.read(list + at + 3 + std::uint32_t(i) * layout.size, v);
            layout.indexedSpan(list + at + 3 + std::uint32_t(i) * layout.size, mesh.arraysLo, mesh.arraysHi);
            std::uint8_t* packed = mesh.vertices.data() + (first + i) * stride;
            packer.pack(&v, 1, packed);
            if (perVertexMatrix_) {
                if (v.matrixIndex % 3 || v.matrixIndex >= 30) return MeshMatrixIndex;
                const unsigned slot = v.matrixIndex / 3;
                const float row = static_cast<float>(lit_ ? slot : slot * 4);  // lit: renumbered by group below
                std::memcpy(packed + mesh.layout.slot, &row, sizeof row);
                mesh.slots = static_cast<std::uint16_t>(mesh.slots | 1u << slot);
                for (unsigned u = 0; u < units_; ++u) {
                    if (!texgens_[u].perVertexMatrix) continue;
                    const unsigned index = v.texMatrixIndex[texgens_[u].coord];
                    const std::uint8_t mode = index == v.matrixIndex ? GxMesh::SlotModelView : index == v.matrixIndex + 30u ? GxMesh::SlotPaired : 0;
                    if (!mode || (slotTexgen[u] && slotTexgen[u] != mode)) return MeshTexgen;
                    slotTexgen[u] = mode;
                }
            }
        }
        const auto triangle = [&](int a, int b, int c) {
            for (int k : {a, b, c}) mesh.indices.push_back(static_cast<std::uint16_t>(first + k));
        };
        switch (kind) {
        case 0x80: case 0x88:  // quads
            for (int i = 0; i + 3 < count; i += 4) triangle(i, i + 1, i + 2), triangle(i, i + 2, i + 3);
            break;
        case 0x90:  // triangles
            for (int i = 0; i + 2 < count; i += 3) triangle(i, i + 1, i + 2);
            break;
        case 0x98:  // a strip: every other triangle turns the other way
            for (int i = 0; i + 2 < count; ++i) i & 1 ? triangle(i + 1, i, i + 2) : triangle(i, i + 1, i + 2);
            break;
        case 0xA0:  // a fan
            for (int i = 1; i + 1 < count; ++i) triangle(0, i, i + 1);
            break;
        }
        at += 3 + std::uint32_t(count) * layout.size;
    }
    if (mesh.indices.empty()) return MeshEmpty;
    if (!lit_ || !perVertexMatrix_) {
        mesh.groups.push_back({0, static_cast<std::uint32_t>(mesh.indices.size()), {}, 0});
        mesh.origin.resize(mesh.vertexCount());
        for (std::uint32_t i = 0; i < mesh.origin.size(); ++i) mesh.origin[i] = i;
        return MeshOk;
    }
    // Lit: triangles in order, a new group whenever one would name a slot past the shader's; each group's vertices its
    // own copies, numbered by its compact slots.
    std::vector<std::uint8_t> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::int32_t> copy(mesh.vertexCount(), -1);
    std::vector<std::uint32_t> copied;
    GxMesh::Group group;
    std::memcpy(group.slotTexgen, slotTexgen, sizeof slotTexgen);
    for (unsigned u = 0; u < units_; ++u) group.slotRows = static_cast<std::uint8_t>(group.slotRows + 3 * (slotTexgen[u] == GxMesh::SlotPaired));
    const unsigned maxSlots = std::min(GxMesh::MaxLitSlots, (58u - 4) / group.slotRows);  // the shader's 58 clip rows
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        std::uint8_t slots[3], fresh = 0;
        for (int k = 0; k < 3; ++k) {
            float row;
            std::memcpy(&row, mesh.vertices.data() + mesh.indices[t + k] * stride + mesh.layout.slot, sizeof row);
            slots[k] = static_cast<std::uint8_t>(row);
            bool known = std::find(group.slotMap, group.slotMap + group.slotCount, slots[k]) != group.slotMap + group.slotCount;
            for (int j = 0; j < k; ++j) known |= slots[j] == slots[k];
            fresh = static_cast<std::uint8_t>(fresh + !known);
        }
        if (group.slotCount + fresh > maxSlots) {
            group.indexCount = static_cast<std::uint32_t>(indices.size()) - group.firstIndex;
            mesh.groups.push_back(group);
            group.slotCount = 0;
            group.firstIndex = static_cast<std::uint32_t>(indices.size());
            for (std::uint32_t v : copied) copy[v] = -1;
            copied.clear();
        }
        for (int k = 0; k < 3; ++k) {
            const std::uint16_t source = mesh.indices[t + k];
            if (copy[source] < 0) {
                if (vertices.size() / stride == 0x10000) return MeshTooLarge;
                unsigned slot = 0;
                while (slot < group.slotCount && group.slotMap[slot] != slots[k]) ++slot;
                if (slot == group.slotCount) group.slotMap[group.slotCount++] = slots[k];
                copy[source] = static_cast<std::int32_t>(vertices.size() / stride), copied.push_back(source);
                const std::uint8_t* from = mesh.vertices.data() + source * stride;
                vertices.insert(vertices.end(), from, from + stride);
                mesh.origin.push_back(source);
                const float row = static_cast<float>(4 + slot * group.slotRows);
                std::memcpy(vertices.data() + vertices.size() - stride + mesh.layout.slot, &row, sizeof row);
            }
            indices.push_back(static_cast<std::uint16_t>(copy[source]));
        }
    }
    group.indexCount = static_cast<std::uint32_t>(indices.size()) - group.firstIndex;
    mesh.groups.push_back(group);
    mesh.vertices = std::move(vertices), mesh.indices = std::move(indices);
    return MeshOk;
}

namespace {
// The attributes a packed vertex is made from.
std::uint16_t meshReads(const GxMeshLayout& layout) {
    std::uint16_t mask = GxVertexLayout::ReadPosition | (layout.color != GxMeshLayout::Absent ? GxVertexLayout::ReadColor0 : 0) |
                         (layout.normal != GxMeshLayout::Absent ? GxVertexLayout::ReadNormal : 0);
    for (const GxMeshLayout::Unit& unit : layout.unit)
        if (unit.input == GxMeshLayout::Stored && unit.source >= 5) mask = static_cast<std::uint16_t>(mask | 1u << (unit.source - 1));
        else if (unit.input == GxMeshLayout::Stored && unit.source == 1) mask |= GxVertexLayout::ReadNormal;
    return mask;
}

// A format's vertices made into a layout's packed ones with no decoded vertex between: each attribute is fetched to its
// first place, then patches zero what the format lacks (a build's vertex starts zero), copy second places, and set the one.
struct MeshFetch {
    struct Fetch {
        GxVertexLayout::Plan::Op::Run run;
        const void* attribute;
        std::uint8_t offset, to;  // where in the list's vertex, and in the packed one
    };
    struct Patch {
        enum Kind : std::uint8_t { Zero, Copy, One };
        Kind kind;
        std::uint8_t to, from, words;
    };
    Fetch fetches[12];
    Patch patches[16];  // at most a zero or copy for each of six places, a zero for each of six attributes, a one for each of three units
    unsigned fetched, patched;

    void make(const GxVertexLayout& layout, const GxMeshLayout& packing, std::uint16_t mask) {
        const GxVertexLayout::Plan plan = layout.plan(mask);
        const GxVertexLayout::Plan::Op* planned[12] = {};
        for (const GxVertexLayout::Plan::Op* op = plan.ops; op != plan.ops + plan.count; ++op) {
            const unsigned to = op->to;
            planned[to == offsetof(GxVertex, position) ? 0 : to == offsetof(GxVertex, normal) ? 1 : to == offsetof(GxVertex, color) ? 2 : 4 + (to - offsetof(GxVertex, texcoord)) / 8] = op;
        }
        fetched = patched = 0;
        for (unsigned a = 0; a < 12; ++a) {
            if (!(mask >> a & 1)) continue;
            // Where the packed vertex holds attribute a, and how many words of it (a unit's input is two components, then maybe a one).
            struct Place { std::uint8_t to, words; bool one; } places[4];
            unsigned n = 0;
            if (a == 0) places[n++] = {0, 3, false};
            if (a == 1 && packing.normal != GxMeshLayout::Absent) places[n++] = {packing.normal, 3, false};
            if (a == 2) places[n++] = {packing.color, 1, false};
            for (const GxMeshLayout::Unit& unit : packing.unit)
                if (unit.input == GxMeshLayout::Stored && (unit.source < 2 ? unit.source : unit.source - 1u) == a) places[n++] = {unit.at, 2, unit.floats == 3};
            const unsigned width = planned[a] ? plan.width[a] : 0;
            if (planned[a]) fetches[fetched++] = {planned[a]->run, planned[a]->attribute, planned[a]->offset, places[0].to};
            if (width < places[0].words) patches[patched++] = {Patch::Zero, static_cast<std::uint8_t>(places[0].to + 4 * width), 0, static_cast<std::uint8_t>(places[0].words - width)};
            for (unsigned k = 1; k < n; ++k) patches[patched++] = {Patch::Copy, places[k].to, places[0].to, places[k].words};
            for (unsigned k = 0; k < n; ++k)
                if (places[k].one) patches[patched++] = {Patch::One, static_cast<std::uint8_t>(places[k].to + 8), 0, 1};
        }
    }

    // `count` vertices, each at vertices[i], into `out` a stride apart.
    void run(const std::uint8_t* const* vertices, unsigned count, std::uint8_t* out, std::size_t stride) const {
        for (const Fetch* fetch = fetches; fetch != fetches + fetched; ++fetch)
            fetch->run(fetch->attribute, vertices, fetch->offset, out + fetch->to, stride, count);
        static constexpr float One = 1;
        static constexpr std::uint32_t Zero = 0;
        for (const Patch* patch = patches; patch != patches + patched; ++patch) {
            std::uint8_t* to = out + patch->to;
            if (patch->kind == Patch::Zero)
                for (unsigned i = 0; i < count; ++i, to += stride)
                    for (unsigned w = 0; w < patch->words; ++w) std::memcpy(to + 4 * w, &Zero, 4);
            else if (patch->kind == Patch::Copy)
                for (unsigned i = 0; i < count; ++i, to += stride)
                    for (unsigned w = 0; w < patch->words; ++w) std::memcpy(to + 4 * w, to - patch->to + patch->from + 4 * w, 4);
            else
                for (unsigned i = 0; i < count; ++i, to += stride) std::memcpy(to, &One, 4);
        }
    }
};

// Each vertex of `out` is the scratch's vertex its origin names, its slot kept from `built`. A vertex is whole words, and
// newlib's memcpy can go a byte at a time: with the size known (Words, or 0 for any) the copy is a few ldm and stm.
template <unsigned Words>
bool gatherVertices(const std::uint32_t* origin, std::size_t count, std::size_t decoded, std::size_t stride, const std::uint8_t* scratch, const std::uint8_t* built,
                    std::uint8_t* out, unsigned slot) {
    const std::size_t bytes = Words ? 4 * Words : stride;
    for (std::size_t i = 0; i < count; ++i, out += bytes, built += bytes) {
        if (origin[i] >= decoded) return false;
        const std::uint8_t* from = scratch + origin[i] * bytes;
        std::uint8_t* to = out;
        if constexpr (Words) {
            std::memcpy(__builtin_assume_aligned(to, 4), __builtin_assume_aligned(from, 4), bytes);
        } else {
            for (std::size_t k = 0; k < bytes; k += 4) std::memcpy(to + k, from + k, 4);
        }
        if (slot != GxMeshLayout::Absent) std::memcpy(out + slot, built + slot, sizeof(float));
    }
    return true;
}
} // namespace

bool GxVertexPipeline::refreshMesh(const std::uint8_t* list, std::uint32_t size, const GxMeshLayout& packing, const std::uint32_t* origin,
                                   const std::uint8_t* built, std::uint8_t* out, std::size_t count) const {
    const std::uint16_t mask = meshReads(packing);
    const std::size_t stride = packing.stride;
    // An unlit mesh keeps its list's vertices in order, decoded straight into place; a lit one's groups copy them in
    // triangle order, gathered from a scratch kept between refreshes (moving meshes refresh every frame).
    std::size_t ordered = 0;
    while (origin && ordered < count && origin[ordered] == ordered) ++ordered;
    const bool inOrder = !origin || ordered == count;
    thread_local std::vector<std::uint8_t> scratch;
    // Vertices are fetched RefreshChunk at a time, from wherever each starts, in the format of the primitive that began the chunk.
    constexpr unsigned RefreshChunk = 128;
    const std::uint8_t* vertices[RefreshChunk];
    const GxVertexLayout* layouts[8] = {};
    MeshFetch fetch;
    unsigned format = 8, filled = 0;
    std::size_t decoded = 0;
    const auto flush = [&] {
        if (!filled) return;
        if (!inOrder && scratch.size() < (decoded + filled) * stride) scratch.resize((decoded + filled) * stride);
        std::uint8_t* const into = (inOrder ? out : scratch.data()) + decoded * stride;
        fetch.run(vertices, filled, into, stride);
        if (inOrder && packing.slot != GxMeshLayout::Absent) {
            std::uint8_t* to = out + decoded * stride + packing.slot;
            const std::uint8_t* from = built + decoded * stride + packing.slot;
            for (unsigned k = 0; k < filled; ++k, to += stride, from += stride) std::memcpy(to, from, sizeof(float));
        }
        decoded += filled, filled = 0;
    };
    for (std::uint32_t at = 0; at < size;) {
        const std::uint8_t op = list[at];
        if (op == 0x00) {
            ++at;
            continue;
        }
        if (op < 0x80 || op >= 0xA8 || at + 3 > size) return false;
        const std::uint16_t primitive = static_cast<std::uint16_t>(list[at + 1] << 8 | list[at + 2]);
        const unsigned f = op & 7;
        if (!layouts[f]) layouts[f] = &gpu_->layout(static_cast<std::uint8_t>(f));
        const std::uint32_t vertexSize = layouts[f]->size;  // read once: the stores below could alias it
        if (at + 3 + std::uint32_t(primitive) * vertexSize > size) return false;
        std::size_t take = inOrder ? std::min<std::size_t>(primitive, count - decoded - filled) : primitive;
        if (take && f != format) {
            flush();
            fetch.make(*layouts[f], packing, mask);
            format = f;
        }
        for (const std::uint8_t* vertex = list + at + 3; take;) {
            const std::size_t run = std::min<std::size_t>(take, RefreshChunk - filled);
            for (std::size_t k = 0; k < run; ++k, vertex += vertexSize) vertices[filled + k] = vertex;
            filled += static_cast<unsigned>(run), take -= run;
            if (filled == RefreshChunk) flush();
        }
        at += 3 + std::uint32_t(primitive) * vertexSize;
    }
    flush();
    if (inOrder) return decoded >= count;
    const std::uint8_t* const from = scratch.data();
    switch (stride / 4) {
    case 6: return gatherVertices<6>(origin, count, decoded, stride, from, built, out, packing.slot);
    case 8: return gatherVertices<8>(origin, count, decoded, stride, from, built, out, packing.slot);
    case 9: return gatherVertices<9>(origin, count, decoded, stride, from, built, out, packing.slot);
    case 10: return gatherVertices<10>(origin, count, decoded, stride, from, built, out, packing.slot);
    case 11: return gatherVertices<11>(origin, count, decoded, stride, from, built, out, packing.slot);
    default: return gatherVertices<0>(origin, count, decoded, stride, from, built, out, packing.slot);
    }
}

GxMeshLayout GxVertexPipeline::meshLayout() const {
    GxMeshLayout::Texgen texgens[CombinerProgram::MaxUnits] = {};
    for (unsigned u = 0; u < units_; ++u) texgens[u] = {texgens_[u].source, texgens_[u].threeInputs, texgens_[u].perVertexMatrix};
    return GxMeshLayout::make(meshColorFromVertex(), lit_, perVertexMatrix_, units_, texgens);
}

std::uint32_t GxVertexPipeline::meshSources() const {
    std::uint32_t key = units_;
    for (unsigned u = 0; u < units_; ++u)
        key |= std::uint32_t(texgens_[u].source | texgens_[u].threeInputs << 4 | texgens_[u].perVertexMatrix << 5) << (2 + 8 * u);
    return key;
}

void GxVertexPipeline::meshSlotUniforms(const GxMesh::Group& group, GxMeshUniforms& out) {
    // View to clip, then compact slots: the modelview rows and the normal rows (XF 0x400, three of three per slot).
    out.slots = 0;
    std::memcpy(out.toClip, toClipFloat_, sizeof out.toClip);
    std::memcpy(out.slotTexgen, group.slotTexgen, sizeof out.slotTexgen);
    out.slotRows = group.slotRows;
    unsigned paired = 0;
    for (unsigned u = 0; u < CombinerProgram::MaxUnits; ++u)
        out.slotTexgenRow[u] = group.slotTexgen[u] == GxMesh::SlotPaired ? static_cast<float>(6 + 3 * paired++) : 0.0f;
    const unsigned count = perVertexMatrix_ ? group.slotCount : 1;
    for (unsigned k = 0; k < count; ++k) {
        const std::uint32_t row = perVertexMatrix_ ? group.slotMap[k] * 3u : defaultMatrix_;
        copyRows(out.modelView[k], gpu_->xf, row, 3);
        const std::uint32_t normals = 0x400 + (row & 31) * 3;
        std::memcpy(out.normal[k], gpu_->xf + normals, sizeof out.normal[k]);
        for (unsigned u = 0, p = 0; u < units_; ++u)
            if (group.slotTexgen[u] == GxMesh::SlotPaired) copyRows(out.paired[k][p++], gpu_->xf, row + 30, 3);
        out.slots |= 1u << k;
    }
}

void GxVertexPipeline::meshUniforms(const GxMeshLayout& layout, std::uint16_t slots, const GxMesh::Group& group, GxMeshUniforms& out) {
    out.slots = 0;
    out.lit = lit_;
    if (lit_) {
        meshSlotUniforms(group, out);
        out.rgbLit = lighting_.parts[0].lit, out.alphaLit = lighting_.parts[1].lit;
        out.rgbAmbientFromVertex = lighting_.parts[0].ambientFromVertex, out.alphaAmbientFromVertex = lighting_.parts[1].ambientFromVertex;
        out.lights = std::min(lightCount_, 4u);
        std::memcpy(out.light, lightRows_, out.lights * sizeof out.light[0]);
        std::memcpy(out.ambient, ambientRow_, sizeof out.ambient);
    } else if (perVertexMatrix_) {
        for (unsigned slot = 0; slot < 10; ++slot)
            if (slots >> slot & 1) std::memcpy(out.clip[slot], clipMatrix(slot * 3), sizeof out.clip[slot]), out.slots |= 1u << slot;
    } else {
        std::memcpy(out.clip[0], clipMatrix(defaultMatrix_), sizeof out.clip[0]);
        out.slots = 1;
    }
    out.units = units_;
    for (unsigned u = 0; u < units_; ++u) {
        const TexGen& t = texgens_[u];
        if (group.slotTexgen[u]) {
            // The dual transform alone; with two rows the texgen's third output is one, whatever the slot's third row says.
            if (t.dual) copyRows(&out.texgen[u][0][0], gpu_->xf, t.post, 3);
            else
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c) out.texgen[u][r][c] = float(r == c);
            if (t.rows == 2)
                for (auto& row : out.texgen[u]) row[3] += row[2], row[2] = 0;
        } else {
            std::memcpy(out.texgen[u], t.fold, sizeof out.texgen[u]);
            // The input's third is one, and the shader fills in zero for the two floats the mesh holds.
            if (layout.unit[u].floats == 2)
                for (auto& row : out.texgen[u]) row[3] += row[2], row[2] = 0;
        }
        const float* q = out.texgen[u][2];
        out.projective[u] = !(q[0] == 0 && q[1] == 0 && q[2] == 0 && q[3] == 1);
    }
    out.fixedColor = !meshColorFromVertex();
    out.rgbFromVertex = !out.fixedColor && lighting_.parts[0].materialFromVertex;
    out.alphaFromVertex = !out.fixedColor && lighting_.parts[1].materialFromVertex;
    if (color0InVertex_) std::memcpy(out.color, color0_, 4);
    else if (!channel_) std::memset(out.color, 0, 4);
    else std::memcpy(out.color, lighting_.material, 4);
    out.reject = reject_;
}

void buildGxDraw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices,
                 const GxFrame& frame, GxDraw& out) {
    std::vector<GxDrawVertex> triangles = std::move(out.triangles);
    triangles.clear();
    readGxDrawState(gpu, out);
    out.triangles = std::move(triangles);
    GxVertexPipeline pipeline;
    if (!pipeline.prepare(gpu, out, frame, kind, format)) return void(out.unsupported = pipeline.unsupported);
    std::vector<GxDrawVertex> drawn(GxVertexPipeline::maxVertices(kind, count));
    std::vector<std::uint16_t> indices(GxVertexPipeline::maxIndices(kind, count));
    const std::size_t n = pipeline.run(vertices, count, drawn.data(), 0, indices.data());
    for (std::size_t i = 0; i < n; ++i) out.triangles.push_back(drawn[indices[i]]);
}
} // namespace smg
