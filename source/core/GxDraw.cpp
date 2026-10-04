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
// A TEV colour register's 11-bit two's complement component, over 255.
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
        if (indirect >> 7 & 3 || indirect >> 13 & 0x3F || indirect >> 20 & 1)
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
    }
    shared = parts[0].lit && parts[1].lit && parts[0].mask == parts[1].mask && parts[0].diffuse == parts[1].diffuse &&
             parts[0].attenuated == parts[1].attenuated && parts[0].spot == parts[1].spot;
    for (int n = 0; n < 8; ++n) {
        if (!(used >> n & 1)) continue;
        const std::uint32_t* w = gpu.xf + XfLights + 16 * n;  // three reserved words, colour, A0-A2, K0-K2, position, direction
        Light& l = lights[n];
        for (int c = 0; c < 4; ++c) l.color[c] = static_cast<float>(w[3] >> (24 - 8 * c) & 0xFF);
        for (int k = 0; k < 3; ++k)
            l.cosine[k] = asFloat(w[4 + k]), l.distance[k] = asFloat(w[7 + k]), l.position[k] = asFloat(w[10 + k]), l.direction[k] = asFloat(w[13 + k]);
    }
}

void GxLighting::apply(const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]) const {
    float sums[2][4];
    for (int i = 0; i < 2; ++i)
        for (int c = 0; c < 4; ++c) sums[i][c] = parts[i].ambientFromVertex ? vertex[c] : ambient[c];
    if (shared) {
        for (int n = 0; n < 8; ++n)
            if (parts[0].mask >> n & 1) addLight(lights[n], parts[0], view, normal, sums[0], 0, 3, &sums[1][3]);
    } else {
        for (int i = 0; i < 2; ++i)
            if (parts[i].lit)
                for (int n = 0; n < 8; ++n)
                    if (parts[i].mask >> n & 1) addLight(lights[n], parts[i], view, normal, sums[i], i ? 3 : 0, i ? 4 : 3, nullptr);
    }
    for (int i = 0; i < 2; ++i) {
        // Part 0 is the colour (RGB), part 1 the alpha.
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

void gxMeshModel(const GxMeshUniforms& u, const GxMeshVertex& v, float clip[4], float color[4], float tex[CombinerProgram::MaxUnits][2]) {
    const auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto dot4 = [](const float* row, const float* xyz) { return row[0] * xyz[0] + row[1] * xyz[1] + row[2] * xyz[2] + row[3]; };
    const unsigned slot = u.lit ? (static_cast<unsigned>(v.slot) - 4) / 6 : static_cast<unsigned>(v.slot) / 4;
    float view[3];
    if (u.lit) {
        // The slot's modelview, then the shared rows to clip space, as the shader does.
        for (int r = 0; r < 3; ++r) view[r] = dot4(u.modelView[slot] + r * 4, v.position);
        for (int r = 0; r < 4; ++r) clip[r] = dot4(u.toClip + r * 4, view);
    } else {
        for (int r = 0; r < 4; ++r) clip[r] = dot4(u.clip[slot] + r * 4, v.position);
    }
    for (unsigned t = 0; t < u.units; ++t) {
        const float s = dot4(u.texgen[t][0], v.source[t]), w = dot4(u.texgen[t][1], v.source[t]);
        const float q = u.projective[t] ? dot4(u.texgen[t][2], v.source[t]) : 1;
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
    std::uint8_t count;
    TevStage stages[16];
};

std::uint32_t tevCompiles = 0;

const CombinerProgram& cachedTev(const ModelMaterial& m, bool color1FromUnit = false) {
    TevKey key;
    std::memset(static_cast<void*>(&key), 0, sizeof key);
    key.color1FromUnit = color1FromUnit;
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
    entry.program = compileTev(m, color1FromUnit);
    return entry.program;
}
} // namespace

std::uint32_t gxTevCompiles() { return tevCompiles; }

namespace {
// TEV register `reg` (1 C0, 2 C1) as four bytes: BP 0xE0 + 2 reg and the next, eleven-bit two's complement components.
bool tevColorBytes(const GxCommandProcessor& gpu, unsigned reg, std::uint8_t out[4]) {
    const std::uint32_t ra = gpu.tevColor[2 * reg], bg = gpu.tevColor[2 * reg + 1];
    const std::int32_t c[4] = {static_cast<std::int32_t>(TEV_REGISTERL_GET_R(ra) << 21) >> 21, static_cast<std::int32_t>(TEV_REGISTERH_GET_G(bg) << 21) >> 21,
                               static_cast<std::int32_t>(TEV_REGISTERH_GET_B(bg) << 21) >> 21, static_cast<std::int32_t>(TEV_REGISTERL_GET_A(ra) << 21) >> 21};
    for (int i = 0; i < 4; ++i) {
        if (c[i] < 0 || c[i] > 255) return false;
        out[i] = static_cast<std::uint8_t>(c[i]);
    }
    return true;
}
} // namespace

bool gxColor0Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]) { return tevColorBytes(gpu, 1, out); }
bool gxColor1Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]) { return tevColorBytes(gpu, 2, out); }

namespace {
// C0 can ride in the vertex colour when the program reads it and never writes it, never reads the rasterized colour
// (whose place it takes), C0 is a byte colour, and a swap table leaves the rasterized colour as it is.
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
    unsigned registers = 0, written = 0;
    bool konst = false, raster = false;
    for (unsigned i = 0; i < stages; ++i) {
        const std::uint32_t color = gpu.bp[0xC0 + 2 * i], alpha = gpu.bp[0xC1 + 2 * i];
        for (std::uint32_t in : {TEV_COLOR_ENV_GET_SELA(color), TEV_COLOR_ENV_GET_SELB(color), TEV_COLOR_ENV_GET_SELC(color), TEV_COLOR_ENV_GET_SELD(color)}) {
            if (in < 8) registers |= 1u << (in / 2);
            konst |= in == 14, raster |= in == 10 || in == 11;
        }
        for (std::uint32_t in : {TEV_ALPHA_ENV_GET_SELA(alpha), TEV_ALPHA_ENV_GET_SELB(alpha), TEV_ALPHA_ENV_GET_SELC(alpha), TEV_ALPHA_ENV_GET_SELD(alpha)}) {
            if (in < 4) registers |= 1u << in;
            konst |= in == 6, raster |= in == 5;
        }
        written |= 1u << TEV_COLOR_ENV_GET_DEST(color) | 1u << TEV_ALPHA_ENV_GET_DEST(alpha);
    }
    // A C0 riding in the vertex colour (color0CanRide), or a C1 in the palette (color1CanRide), is read where it is drawn:
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

void readGxDrawState(const GxCommandProcessor& gpu, GxDrawState& out, bool palette) {
    out = GxDrawState{};
    readTev(gpu, out);
    readPixel(gpu, out);
    std::uint8_t identitySwap = 0;
    if (!color0CanRide(gpu, out.material, identitySwap)) {
        out.program = cachedTev(out.material);
        return;
    }
    // Compiled with C0 read as the rasterized colour and its value left out, so every C0 shares one program.
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

float GxVertexPipeline::matrix(std::uint32_t row, int column) const { return asFloat(gpu_->xf[(row * 4 + column) & 0xFFF]); }

bool GxVertexPipeline::prepare(const GxCommandProcessor& gpu, const GxDrawState& state, const GxFrame& frame, std::uint8_t kind, std::uint8_t format) {
    // Of the CP registers, only the descriptor's matrix-index bits are read; the kind and format are only kept.
    if (prepared_ && &gpu == gpu_ && &state == state_ && kind < 0xA8 && gpu.cp[0x50] == preparedDescriptor_ && frame == preparedFrame_ &&
        gpu.bpChangesBesideColors == preparedBeside_ && gpu.xfRegisterChanges == preparedXfRegisters_) {
        // Only TEV colours or XF matrix memory changed (a particle's): C0's bytes, the folds and the clip matrices again.
        if (gpu.bpChanges != preparedBp_) {
            if (color0InVertex_ && !gxColor0Bytes(gpu, color0_)) {
                unsupported = "C0 outside a byte while it rides in the vertex colour";
                return prepared_ = false;
            }
            preparedBp_ = gpu.bpChanges;
        }
        if (gpu.xfChanges != preparedXf_) {
            for (unsigned u = 0; u < units_; ++u)
                if (!texgens_[u].palette) refold(texgens_[u]);
            if (channel_ && lightingLights_ != gpu.xfLightChanges) readLighting(gpu);
            preparedXf_ = xfSeen_ = gpu.xfChanges, clipReady_ = 0;
        }
        kind_ = kind, preparedFormat_ = format;
        return true;
    }
    prepared_ = false, ++fullPrepares;
    gpu_ = &gpu, state_ = &state, kind_ = kind;
    unsupported = state.unsupported;
    if (kind >= 0xA8) unsupported = "lines and points";
    if (unsupported) return false;
    // GEN_MODE's reject bits as GXSetCullMode swaps them in: 1 culls what GX_CULL_BACK names, 2 GX_CULL_FRONT.
    reject_ = static_cast<std::uint8_t>(GEN_MODE_GET_REJECT_EN(gpu.bp[0x00]));
    perVertexMatrix_ = gpu.cp[0x50] & 1;
    defaultMatrix_ = gpu.xf[XfMatIndexA] & 0x3F;
    channel_ = (gpu.xf[XfNumChans] & 3) != 0;
    color0InVertex_ = state.color0InVertex;
    if (color0InVertex_ && !gxColor0Bytes(gpu, color0_)) {
        unsupported = "C0 outside a byte while it rides in the vertex colour";
        return false;
    }
    lit_ = channel_ && (XF_COLOR0CNTRL_F_GET_LIGHTFUNC(gpu.xf[XfColor0Ctrl]) || XF_COLOR0CNTRL_F_GET_LIGHTFUNC(gpu.xf[XfAlpha0Ctrl]));
    fixedDepth_ = state.fixedDepth, depth01_ = state.depth01;
    units_ = static_cast<std::uint8_t>(state.program.unitCount);
    for (unsigned u = 0; u < units_; ++u) {
        TexGen& t = texgens_[u];
        t.palette = state.program.units[u].texture == CombinerProgram::Palette;
        if (t.palette) continue;
        const std::uint8_t coord = state.program.units[u].texCoord;
        const std::uint32_t ctrl = gpu.xf[XfTexGen + coord];
        const std::uint32_t row = XF_TEX_GET_SOURCE_ROW(ctrl);
        if (XF_TEX_GET_TEXGEN_TYPE(ctrl) != 0) unsupported = "emboss and colour texgens";
        else if (row != 0 && row != 1 && (row < 5 || row > 12)) unsupported = "texgens from colours or binormals";
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
        t.ctrl = ctrl, t.postCtrl = post;
        t.folded = !t.perVertexMatrix && !(t.dual && t.normalize);
        refold(t);
    }
    if (channel_ && (lightingRegisters_ != gpu.xfRegisterChanges || lightingLights_ != gpu.xfLightChanges)) readLighting(gpu);
    prepared_ = true, preparedFormat_ = format, preparedFrame_ = frame, preparedDescriptor_ = gpu.cp[0x50];
    preparedBp_ = gpu.bpChanges, preparedXf_ = gpu.xfChanges;
    preparedBeside_ = gpu.bpChangesBesideColors, preparedXfRegisters_ = gpu.xfRegisterChanges;
    if (gpu.xfChanges == xfSeen_ && frame == frameSeen_) return true;
    xfSeen_ = gpu.xfChanges, clipReady_ = 0;
    // Most XF loads are matrices; the projection and viewport change far less often.
    std::uint32_t view[13];
    std::memcpy(view, gpu.xf + XfViewport, sizeof view);
    if (!(frame == frameSeen_) || std::memcmp(view, viewSeen_, sizeof view)) {
        frameSeen_ = frame;
        std::memcpy(viewSeen_, view, sizeof view);
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
        }
    }
    return true;
}

// A primitive's texgen usually reuses the last one's matrices: their words are compared rather than folded again.
bool GxVertexPipeline::foldStillHolds(TexGen& t, std::uint32_t ctrl, std::uint32_t post) const {
    if (t.foldXf == gpu_->xfChanges && t.sourceWords && t.source_[0] == t.coord && t.source_[1] == ctrl) return true;
    // A load that left the fold's own setup and rows as they were (most often a particle's position matrix).
    const std::uint32_t* rows = gpu_->xfRowChanges;
    bool same = t.sourceWords == 5u + 4u * t.rows + (t.dual ? 12u : 0u) && t.source_[0] == t.coord && t.source_[1] == ctrl &&
                t.source_[2] == t.dual && t.source_[3] == post && t.source_[4] == t.matrix;
    for (int r = 0; r < t.rows && same; ++r) same = rows[(t.matrix + r) & 0x3FF] == t.rowChanges[r];
    for (int r = 0; r < 3 && same && t.dual; ++r) same = rows[(t.post + r) & 0x3FF] == t.rowChanges[3 + r];
    if (same) return t.foldXf = gpu_->xfChanges, true;
    std::uint32_t words[32];
    std::uint32_t n = 0;
    words[n++] = t.coord, words[n++] = ctrl, words[n++] = t.dual, words[n++] = post, words[n++] = t.matrix;
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < 4; ++c) words[n++] = gpu_->xf[((t.matrix + r) * 4 + c) & 0xFFF];
    if (t.dual)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) words[n++] = gpu_->xf[((t.post + r) * 4 + c) & 0xFFF];
    same = n == t.sourceWords && !std::memcmp(words, t.source_, n * 4);
    std::memcpy(t.source_, words, n * 4);
    t.sourceWords = n, t.foldXf = gpu_->xfChanges;
    for (int r = 0; r < t.rows; ++r) t.rowChanges[r] = rows[(t.matrix + r) & 0x3FF];
    for (int r = 0; r < 3 && t.dual; ++r) t.rowChanges[3 + r] = rows[(t.post + r) & 0x3FF];
    return same;
}

// A folded texgen's map again when the matrices it was made from changed, and whether it is planar.
void GxVertexPipeline::refold(TexGen& t) const {
    if (t.folded && !foldStillHolds(t, t.ctrl, t.postCtrl)) {
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
                t.fold[r][c] = static_cast<float>(sum);
            }
    }
    // A texcoord source, or the AB11 form, gives a third input of one; with q fixed at one there's nothing to divide.
    t.planar = t.folded && (t.source >= 5 || !t.threeInputs) && t.fold[2][0] == 0 && t.fold[2][1] == 0 && t.fold[2][2] == 0 && t.fold[2][3] == 1;
}

// A position matrix (three rows from `index`) through toClip_: one 4x4 per vertex instead of a matrix, the projection
// and the viewport.
const float* GxVertexPipeline::makeClipMatrix(std::uint32_t index) {
    float* out = index < 64 ? clip_[index] : clipScratch_;
    double m[3][4];
    for (int k = 0; k < 3; ++k)
        for (int c = 0; c < 4; ++c) m[k][c] = matrix(index + k, c);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double sum = c == 3 ? toClip_[r][3] : 0;  // the position matrix's fourth row is (0, 0, 0, 1)
            for (int k = 0; k < 3; ++k) sum += toClip_[r][k] * m[k][c];
            out[r * 4 + c] = static_cast<float>(sum);
        }
    if (index < 64) clipReady_ |= std::uint64_t(1) << index;
    return out;
}

// One texture coordinate set: the texgen's input through its matrix, then the dual transform when it is on.
void GxVertexPipeline::texgen(const TexGen& t, const GxVertex& v, float st[2]) const {
    if (t.planar) {
        const float* in = t.source == 0 ? v.position : t.source == 1 ? v.normal : v.texcoord[t.source - 5];
        st[0] = t.fold[0][0] * in[0] + t.fold[0][1] * in[1] + (t.fold[0][2] + t.fold[0][3]);
        st[1] = t.fold[1][0] * in[0] + t.fold[1][1] * in[1] + (t.fold[1][2] + t.fold[1][3]);
        return;
    }
    float in[4] = {0, 0, 1, 1};
    if (t.source == 0) std::memcpy(in, v.position, sizeof v.position);
    else if (t.source == 1) std::memcpy(in, v.normal, sizeof v.normal);
    else in[0] = v.texcoord[t.source - 5][0], in[1] = v.texcoord[t.source - 5][1];
    if (!t.threeInputs) in[2] = 1;
    float c[3];
    if (t.folded) {
        for (int r = 0; r < 3; ++r) c[r] = t.fold[r][0] * in[0] + t.fold[r][1] * in[1] + t.fold[r][2] * in[2] + t.fold[r][3];
        if (std::fabs(c[2] - 1) > 1e-5f && c[2] != 0) st[0] = c[0] / c[2], st[1] = c[1] / c[2];
        else st[0] = c[0], st[1] = c[1];
        return;
    }
    const std::uint32_t index = t.perVertexMatrix ? v.texMatrixIndex[t.coord] : t.matrix;
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

std::size_t GxVertexPipeline::run(const std::uint8_t* data, std::uint16_t count, GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices) {
    const GxVertexLayout& layout = gpu_->layout(preparedFormat_);  // a mesh's draw never needs it, so prepare leaves it
    const std::uint32_t size = layout.size;
    GxVertex v;  // attributes the format lacks stay zero for every vertex
    for (std::uint16_t i = 0; i < count; ++i) {
        layout.read(data + i * size, v);
        vertex(v, vertices[i]);
    }
    return triangles(vertices, count, first, indices);
}

std::size_t GxVertexPipeline::runDecoded(const GxVertex* decoded, std::uint16_t count, GxDrawVertex* vertices, std::uint16_t first, std::uint16_t* indices) {
    for (std::uint16_t i = 0; i < count; ++i) vertex(decoded[i], vertices[i]);
    return triangles(vertices, count, first, indices);
}

inline void GxVertexPipeline::vertex(const GxVertex& v, GxDrawVertex& d) {
    const GxCommandProcessor& gpu = *gpu_;
    const std::uint32_t index = perVertexMatrix_ ? v.matrixIndex : defaultMatrix_;
    const float* m = clipMatrix(index);
    for (int r = 0; r < 4; ++r) d.position[r] = m[r * 4] * v.position[0] + m[r * 4 + 1] * v.position[1] + m[r * 4 + 2] * v.position[2] + m[r * 4 + 3];
    if (fixedDepth_) d.position[2] = (depth01_ - 1) * d.position[3];
    if (color0InVertex_) {
        std::memcpy(d.color, color0_, 4);
    } else if (!channel_) {
        std::memset(d.color, 0, 4);
    } else {
        float view[3] = {0, 0, 0}, normal[3] = {0, 0, 0};
        if (lit_) {
            for (int r = 0; r < 3; ++r)
                view[r] = matrix(index + r, 0) * v.position[0] + matrix(index + r, 1) * v.position[1] + matrix(index + r, 2) * v.position[2] + matrix(index + r, 3);
            // Normal matrices: three rows of three from XF 0x400, at the position matrix's row.
            const std::uint32_t base = 0x400 + (index & 31) * 3;
            for (int r = 0; r < 3; ++r)
                normal[r] = asFloat(gpu.xf[base + r * 3]) * v.normal[0] + asFloat(gpu.xf[base + r * 3 + 1]) * v.normal[1] +
                            asFloat(gpu.xf[base + r * 3 + 2]) * v.normal[2];
            normalize(normal);
        }
        lighting_.apply(view, normal, v.color[0], d.color);
    }
    for (unsigned u = 0; u < units_; ++u) {
        if (texgens_[u].palette) d.texcoord[u][0] = palette_[0], d.texcoord[u][1] = palette_[1];
        else texgen(texgens_[u], v, d.texcoord[u]);
    }
    if (atlasUnit_ >= 0) {
        float* st = d.texcoord[atlasUnit_];
        constexpr float slack = 1.0f / 4096;
        atlasOutside_ |= st[0] < -slack || st[0] > 1 + slack || st[1] < -slack || st[1] > 1 + slack;
        st[0] = st[0] * atlasPlace_[0] + atlasPlace_[2], st[1] = st[1] * atlasPlace_[1] + atlasPlace_[3];
    }
}

std::size_t GxVertexPipeline::triangles(GxDrawVertex* vertices, std::uint16_t count, std::uint16_t first, std::uint16_t* indices) const {
    // Culling in EFB space, by the sign of each triangle's area there: with every w positive that is the sign of the
    // determinant of its (x, y, w) rows, negated (the EFB's y runs down); the screen's quarter turn keeps it.
    std::size_t n = 0;
    const auto emit = [&](int a, int b, int c) {
        const float* pa = vertices[a].position, *pb = vertices[b].position, *pc = vertices[c].position;
        if (reject_ && pa[3] > 0 && pb[3] > 0 && pc[3] > 0) {
            const float det = pa[0] * (pb[1] * pc[3] - pc[1] * pb[3]) - pb[0] * (pa[1] * pc[3] - pc[1] * pa[3]) + pc[0] * (pa[1] * pb[3] - pb[1] * pa[3]);
            if (reject_ == 3 || (reject_ == 1 && det > 0) || (reject_ == 2 && det < 0)) return;
        }
        indices[n++] = static_cast<std::uint16_t>(first + a), indices[n++] = static_cast<std::uint16_t>(first + b), indices[n++] = static_cast<std::uint16_t>(first + c);
    };
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
    return n;
}

namespace {
// The shader's light entries: a light under one part's settings, feeding the colour, the alpha or both (both only when
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

bool GxVertexPipeline::meshColorFromVertex() const {
    return channel_ && !color0InVertex_ && (lighting_.parts[0].materialFromVertex || lighting_.parts[1].materialFromVertex);
}

unsigned GxVertexPipeline::meshEligible() const {
    if (unsupported) return MeshUnsupported;
    for (unsigned u = 0; u < units_; ++u)
        if (texgens_[u].palette) return MeshUnsupported;  // the palette's texel moves per primitive
    if (fixedDepth_) return MeshDepthTexture;
    if (lit_) {
        // The shader runs up to four light entries, each a light under one part's settings (an actor lights its colour
        // with lights 0 and 1, its alpha with light 2 and other settings).
        const GxLighting::Part& color = lighting_.parts[0];
        const GxLighting::Part& alpha = lighting_.parts[1];
        if (color0InVertex_) return MeshLitColor0;
        if ((color.lit && color.ambientFromVertex) || (alpha.lit && alpha.ambientFromVertex)) return MeshLit;
        LightEntry entries[4];
        if (lightEntries(lighting_, entries) > 4) return MeshLights;
    }
    for (unsigned u = 0; u < units_; ++u)
        if (!texgens_[u].folded || texgens_[u].perVertexMatrix) return MeshTexgen;
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
    const bool fromVertex = meshColorFromVertex();
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
        const std::size_t first = mesh.vertices.size();
        if (first + count > 0x10000) return MeshTooLarge;
        GxVertex v;  // attributes the format lacks stay zero for every vertex
        for (std::uint16_t i = 0; i < count; ++i) {
            layout.read(list + at + 3 + std::uint32_t(i) * layout.size, v);
            layout.indexedSpan(list + at + 3 + std::uint32_t(i) * layout.size, mesh.arraysLo, mesh.arraysHi);
            GxMeshVertex m{};
            std::memcpy(m.position, v.position, sizeof m.position);
            if (fromVertex) std::memcpy(m.color, v.color[0], 4);
            for (unsigned u = 0; u < units_; ++u) {
                const TexGen& t = texgens_[u];
                float* in = m.source[u];
                if (t.source == 0) std::memcpy(in, v.position, 12);
                else if (t.source == 1) std::memcpy(in, v.normal, 12);
                else in[0] = v.texcoord[t.source - 5][0], in[1] = v.texcoord[t.source - 5][1], in[2] = 1;
                if (!t.threeInputs) in[2] = 1;
            }
            std::memcpy(m.normal, v.normal, sizeof m.normal);
            if (perVertexMatrix_) {
                if (v.matrixIndex % 3 || v.matrixIndex >= 30) return MeshMatrixIndex;
                const unsigned slot = v.matrixIndex / 3;
                m.slot = static_cast<float>(lit_ ? slot : slot * 4);  // lit: renumbered by group below
                mesh.slots = static_cast<std::uint16_t>(mesh.slots | 1u << slot);
            } else if (lit_) {
                m.slot = 4;  // the one compact slot (the draw's fixed attribute)
            }
            mesh.vertices.push_back(m);
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
        mesh.origin.resize(mesh.vertices.size());
        for (std::uint32_t i = 0; i < mesh.origin.size(); ++i) mesh.origin[i] = i;
        return MeshOk;
    }
    // Lit: triangles in order, a new group whenever one would name a slot past the shader's; each group's vertices its
    // own copies, numbered by its compact slots.
    std::vector<GxMeshVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::int32_t> copy(mesh.vertices.size(), -1);
    std::vector<std::uint32_t> copied;
    GxMesh::Group group;
    for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
        std::uint8_t slots[3], fresh = 0;
        for (int k = 0; k < 3; ++k) {
            slots[k] = static_cast<std::uint8_t>(mesh.vertices[mesh.indices[t + k]].slot);
            bool known = std::find(group.slotMap, group.slotMap + group.slotCount, slots[k]) != group.slotMap + group.slotCount;
            for (int j = 0; j < k; ++j) known |= slots[j] == slots[k];
            fresh = static_cast<std::uint8_t>(fresh + !known);
        }
        if (group.slotCount + fresh > GxMesh::MaxLitSlots) {
            group.indexCount = static_cast<std::uint32_t>(indices.size()) - group.firstIndex;
            mesh.groups.push_back(group);
            group = GxMesh::Group{};
            group.firstIndex = static_cast<std::uint32_t>(indices.size());
            for (std::uint32_t v : copied) copy[v] = -1;
            copied.clear();
        }
        for (int k = 0; k < 3; ++k) {
            const std::uint16_t source = mesh.indices[t + k];
            if (copy[source] < 0) {
                if (vertices.size() == 0x10000) return MeshTooLarge;
                unsigned slot = 0;
                while (slot < group.slotCount && group.slotMap[slot] != slots[k]) ++slot;
                if (slot == group.slotCount) group.slotMap[group.slotCount++] = slots[k];
                copy[source] = static_cast<std::int32_t>(vertices.size()), copied.push_back(source);
                vertices.push_back(mesh.vertices[source]);
                mesh.origin.push_back(source);
                vertices.back().slot = static_cast<float>(4 + slot * 6);
            }
            indices.push_back(static_cast<std::uint16_t>(copy[source]));
        }
    }
    group.indexCount = static_cast<std::uint32_t>(indices.size()) - group.firstIndex;
    mesh.groups.push_back(group);
    mesh.vertices = std::move(vertices), mesh.indices = std::move(indices);
    return MeshOk;
}

std::uint32_t GxVertexPipeline::meshSources() const {
    std::uint32_t key = units_;
    for (unsigned u = 0; u < units_; ++u) key |= std::uint32_t(texgens_[u].source | texgens_[u].threeInputs << 4) << (2 + 8 * u);
    return key;
}

void GxVertexPipeline::meshSlotUniforms(const GxMesh::Group& group, GxMeshUniforms& out) {
    // View to clip, then compact slots: the modelview rows and the normal rows (XF 0x400, three of three per slot).
    out.slots = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.toClip[r * 4 + c] = static_cast<float>(toClip_[r][c]);
    const unsigned count = perVertexMatrix_ ? group.slotCount : 1;
    for (unsigned k = 0; k < count; ++k) {
        const std::uint32_t row = perVertexMatrix_ ? group.slotMap[k] * 3u : defaultMatrix_;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) out.modelView[k][r * 4 + c] = matrix(row + r, c);
        const std::uint32_t normals = 0x400 + (row & 31) * 3;
        for (int i = 0; i < 9; ++i) out.normal[k][i] = asFloat(gpu_->xf[normals + i]);
        out.slots |= 1u << k;
    }
}

void GxVertexPipeline::meshUniforms(std::uint16_t slots, const GxMesh::Group& group, GxMeshUniforms& out) {
    out.slots = 0;
    out.lit = lit_;
    if (lit_) {
        meshSlotUniforms(group, out);
        out.rgbLit = lighting_.parts[0].lit, out.alphaLit = lighting_.parts[1].lit;
        LightEntry entries[4];
        out.lights = std::min(lightEntries(lighting_, entries), 4u);
        for (unsigned e = 0; e < out.lights; ++e) {
            const GxLighting::Part& part = lighting_.parts[entries[e].part];
            const GxLighting::Light& l = lighting_.lights[entries[e].light];
            float (&packed)[6][4] = out.light[e];
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
        for (int c = 0; c < 4; ++c) out.ambient[c] = lighting_.ambient[c] / 255.0f;
    } else if (perVertexMatrix_) {
        for (unsigned slot = 0; slot < 10; ++slot)
            if (slots >> slot & 1) std::memcpy(out.clip[slot], clipMatrix(slot * 3), sizeof out.clip[slot]), out.slots |= 1u << slot;
    } else {
        std::memcpy(out.clip[0], clipMatrix(defaultMatrix_), sizeof out.clip[0]);
        out.slots = 1;
    }
    out.units = units_;
    for (unsigned u = 0; u < units_; ++u) {
        std::memcpy(out.texgen[u], texgens_[u].fold, sizeof out.texgen[u]);
        const float* q = texgens_[u].fold[2];
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
    std::vector<GxDrawVertex> drawn(count);
    std::vector<std::uint16_t> indices(GxVertexPipeline::maxIndices(count));
    const std::size_t n = pipeline.run(vertices, count, drawn.data(), 0, indices.data());
    for (std::size_t i = 0; i < n; ++i) out.triangles.push_back(drawn[indices[i]]);
}
} // namespace smg
