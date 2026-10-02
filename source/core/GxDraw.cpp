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
    std::uint8_t count;
    TevStage stages[16];
};

std::uint32_t tevCompiles = 0;

const CombinerProgram& cachedTev(const ModelMaterial& m) {
    TevKey key;
    std::memset(static_cast<void*>(&key), 0, sizeof key);
    key.count = std::min<std::uint8_t>(m.tevStageCount, 16);
    std::copy(m.stages, m.stages + key.count, key.stages);
    std::copy(m.tevColors, m.tevColors + 4, key.tevColors);
    std::copy(m.konstColors, m.konstColors + 4, key.konstColors);
    std::memcpy(key.swapTables, m.swapTables, sizeof key.swapTables);
    std::copy(m.textures, m.textures + 8, key.textures);
    const std::size_t length = offsetof(TevKey, stages) + key.count * sizeof(TevStage);
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < length; ++i) hash = (hash ^ reinterpret_cast<const std::uint8_t*>(&key)[i]) * 0x100000001B3ull;
    struct Entry {
        TevKey key;
        CombinerProgram program;
    };
    thread_local std::unordered_map<std::uint64_t, Entry> cache;
    if (auto found = cache.find(hash); found != cache.end() && !std::memcmp(&found->second.key, &key, length)) return found->second.program;
    if (cache.size() > 4096) cache.clear();
    ++tevCompiles;
    Entry& entry = cache[hash];
    entry.key = key;
    entry.program = compileTev(m);
    return entry.program;
}
} // namespace

std::uint32_t gxTevCompiles() { return tevCompiles; }

bool gxColor0Bytes(const GxCommandProcessor& gpu, std::uint8_t out[4]) {
    // BP 0xE2/0xE3 as the material keeps them (C0 is tevColors[0]): eleven-bit two's complement components.
    const std::uint32_t ra = gpu.tevColor[2], bg = gpu.tevColor[3];
    const std::int32_t c[4] = {static_cast<std::int32_t>(TEV_REGISTERL_GET_R(ra) << 21) >> 21, static_cast<std::int32_t>(TEV_REGISTERH_GET_G(bg) << 21) >> 21,
                               static_cast<std::int32_t>(TEV_REGISTERH_GET_B(bg) << 21) >> 21, static_cast<std::int32_t>(TEV_REGISTERL_GET_A(ra) << 21) >> 21};
    for (int i = 0; i < 4; ++i) {
        if (c[i] < 0 || c[i] > 255) return false;
        out[i] = static_cast<std::uint8_t>(c[i]);
    }
    return true;
}

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
} // namespace

void readGxDrawState(const GxCommandProcessor& gpu, GxDrawState& out) {
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
}

float GxVertexPipeline::matrix(std::uint32_t row, int column) const { return asFloat(gpu_->xf[(row * 4 + column) & 0xFFF]); }

bool GxVertexPipeline::prepare(const GxCommandProcessor& gpu, const GxDrawState& state, const GxFrame& frame, std::uint8_t kind, std::uint8_t format) {
    gpu_ = &gpu, state_ = &state, kind_ = kind;
    unsupported = state.unsupported;
    if (kind >= 0xA8) unsupported = "lines and points";
    if (unsupported) return false;
    layout_ = &gpu.layout(format);
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
        t.folded = !t.perVertexMatrix && !(t.dual && t.normalize);
        if (t.folded && !foldStillHolds(t, ctrl, post)) {
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
    if (channel_ && lightingXf_ != gpu.xfChanges) lighting_.read(gpu), lightingXf_ = gpu.xfChanges;
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
    std::uint32_t words[32];
    std::uint32_t n = 0;
    words[n++] = t.coord, words[n++] = ctrl, words[n++] = t.dual, words[n++] = post, words[n++] = t.matrix;
    for (int r = 0; r < t.rows; ++r)
        for (int c = 0; c < 4; ++c) words[n++] = gpu_->xf[((t.matrix + r) * 4 + c) & 0xFFF];
    if (t.dual)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) words[n++] = gpu_->xf[((t.post + r) * 4 + c) & 0xFFF];
    const bool same = n == t.sourceWords && !std::memcmp(words, t.source_, n * 4);
    std::memcpy(t.source_, words, n * 4);
    t.sourceWords = n, t.foldXf = gpu_->xfChanges;
    return same;
}

// A position matrix (three rows from `index`) through toClip_: one 4x4 per vertex instead of a matrix, the projection
// and the viewport.
const float* GxVertexPipeline::makeClipMatrix(std::uint32_t index) {
    float* out = index < 64 ? clip_[index] : clipScratch_;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double sum = c == 3 ? toClip_[r][3] : 0;  // the position matrix's fourth row is (0, 0, 0, 1)
            for (int k = 0; k < 3; ++k) sum += toClip_[r][k] * matrix(index + k, c);
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
    const GxCommandProcessor& gpu = *gpu_;
    const std::uint32_t size = layout_->size;
    GxVertex v;  // attributes the format lacks stay zero for every vertex
    for (std::uint16_t i = 0; i < count; ++i) {
        layout_->read(data + i * size, v);
        const std::uint32_t index = perVertexMatrix_ ? v.matrixIndex : defaultMatrix_;
        const float* m = clipMatrix(index);
        GxDrawVertex& d = vertices[i];
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
        for (unsigned u = 0; u < units_; ++u) texgen(texgens_[u], v, d.texcoord[u]);
    }

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
