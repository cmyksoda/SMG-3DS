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
#include <cmath>
#include <cstring>

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

void readTev(const GxCommandProcessor& gpu, GxDraw& out) {
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

void readPixel(const GxCommandProcessor& gpu, GxDraw& out) {
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

// The masked lights' sum for one control word into `sum` (0-255 per component): each light's colour, attenuated
// (specular or spot, by the control word) and turned by the normal (none, signed or clamped).
void addLights(const GxCommandProcessor& gpu, std::uint32_t ctrl, const float view[3], const float normal[3], float sum[4]) {
    const unsigned mask = XF_COLOR0CNTRL_F_GET_LIGHT0(ctrl) | XF_COLOR0CNTRL_F_GET_LIGHT1(ctrl) << 1 | XF_COLOR0CNTRL_F_GET_LIGHT2(ctrl) << 2 |
                          XF_COLOR0CNTRL_F_GET_LIGHT3(ctrl) << 3 | XF_COLOR0CNTRL_F_GET_LIGHT4(ctrl) << 4 | XF_COLOR0CNTRL_F_GET_LIGHT5(ctrl) << 5 |
                          XF_COLOR0CNTRL_F_GET_LIGHT6(ctrl) << 6 | XF_COLOR0CNTRL_F_GET_LIGHT7(ctrl) << 7;
    const unsigned diffuse = XF_COLOR0CNTRL_F_GET_DIFFUSEATTEN(ctrl);  // GXDiffuseFn: none, sign, clamp
    const bool attenuated = XF_COLOR0CNTRL_F_GET_ATTENENABLE(ctrl), spot = XF_COLOR0CNTRL_F_GET_ATTENSELECT(ctrl);
    for (int n = 0; n < 8; ++n) {
        if (!(mask >> n & 1)) continue;
        const std::uint32_t* l = gpu.xf + XfLights + 16 * n;  // three reserved words, colour, A0-A2, K0-K2, position, direction
        const float cosine[3] = {asFloat(l[4]), asFloat(l[5]), asFloat(l[6])};
        float distance[3] = {asFloat(l[7]), asFloat(l[8]), asFloat(l[9])};
        const float direction[3] = {asFloat(l[13]), asFloat(l[14]), asFloat(l[15])};
        float toLight[3] = {asFloat(l[10]) - view[0], asFloat(l[11]) - view[1], asFloat(l[12]) - view[2]};
        float scale = 1;
        if (attenuated && spot) {
            const float d2 = dot(toLight, toLight), d = std::sqrt(d2);
            if (d > 0) for (float& k : toLight) k /= d;
            const float angle = std::max(0.0f, dot(toLight, direction));
            scale = safeDivide(std::max(0.0f, cosine[0] + cosine[1] * angle + cosine[2] * angle * angle),
                               distance[0] + distance[1] * d + distance[2] * d2);
        } else if (attenuated) {
            // Specular: the light's direction holds the half-angle, and distance attenuation runs on the same angle.
            normalize(toLight);
            const float angle = dot(toLight, normal) >= 0 ? std::max(0.0f, dot(direction, normal)) : 0.0f;
            if (diffuse) normalize(distance);
            scale = safeDivide(std::max(0.0f, cosine[0] + cosine[1] * angle + cosine[2] * angle * angle),
                               distance[0] + distance[1] * angle + distance[2] * angle * angle);
        } else if (!normalize(toLight)) {
            std::memcpy(toLight, normal, sizeof toLight);
        }
        float turn = 1;
        if (diffuse) turn = diffuse == 2 ? std::max(0.0f, dot(toLight, normal)) : dot(toLight, normal);
        const std::uint32_t color = l[3];
        for (int c = 0; c < 4; ++c) sum[c] += static_cast<float>(color >> (24 - 8 * c) & 0xFF) * scale * turn;
    }
}
} // namespace

void gxLitChannel(const GxCommandProcessor& gpu, const float view[3], const float normal[3], const std::uint8_t vertex[4], std::uint8_t out[4]) {
    const std::uint32_t ctrl[2] = {gpu.xf[XfColor0Ctrl], gpu.xf[XfAlpha0Ctrl]}, material = gpu.xf[XfMaterial0], ambient = gpu.xf[XfAmbient0];
    for (int part = 0; part < 2; ++part) {
        // Part 0 is the colour (RGB), part 1 the alpha; each has its own control word.
        const int first = part ? 3 : 0, last = part ? 4 : 3;
        std::uint8_t mat[4], amb[4];
        for (int c = 0; c < 4; ++c) {
            mat[c] = XF_COLOR0CNTRL_F_GET_MATERIAL_SRC(ctrl[part]) ? vertex[c] : static_cast<std::uint8_t>(material >> (24 - 8 * c));
            amb[c] = XF_COLOR0CNTRL_F_GET_AMBIENT_SRC(ctrl[part]) ? vertex[c] : static_cast<std::uint8_t>(ambient >> (24 - 8 * c));
        }
        if (!XF_COLOR0CNTRL_F_GET_LIGHTFUNC(ctrl[part])) {
            for (int c = first; c < last; ++c) out[c] = mat[c];
            continue;
        }
        float sum[4];
        for (int c = 0; c < 4; ++c) sum[c] = amb[c];
        addLights(gpu, ctrl[part], view, normal, sum);
        for (int c = first; c < last; ++c) {
            const int light = std::clamp(static_cast<int>(sum[c]), 0, 255);
            out[c] = static_cast<std::uint8_t>(mat[c] * (light + (light >> 7)) >> 8);
        }
    }
}

namespace {
struct Transformer {
    const GxCommandProcessor& gpu;
    const GxFrame& frame;
    GxDraw& out;

    float matrix(std::uint32_t row, int column) const { return asFloat(gpu.xf[(row * 4 + column) & 0xFFF]); }
    std::uint32_t texMatrixIndex(const GxVertex& v, int coord) const {
        if (gpu.cp[0x50] >> (1 + coord) & 1) return v.texMatrixIndex[coord];
        return coord < 4 ? gpu.xf[XfMatIndexA] >> (6 + 6 * coord) & 0x3F : gpu.xf[XfMatIndexB] >> (6 * (coord - 4)) & 0x3F;
    }

    // One texture coordinate set: the texgen's input through its matrix, then the dual transform when it is on.
    void texgen(const GxVertex& v, int coord, float st[2]) {
        const std::uint32_t ctrl = gpu.xf[XfTexGen + coord];
        if (XF_TEX_GET_TEXGEN_TYPE(ctrl) != 0) {
            out.unsupported = "emboss and colour texgens";
            return;
        }
        const std::uint32_t row = XF_TEX_GET_SOURCE_ROW(ctrl);
        float in[4] = {0, 0, 1, 1};
        if (row == 0) std::memcpy(in, v.position, sizeof v.position);
        else if (row == 1) std::memcpy(in, v.normal, sizeof v.normal);
        else if (row >= 5 && row <= 12) in[0] = v.texcoord[row - 5][0], in[1] = v.texcoord[row - 5][1];
        else { out.unsupported = "texgens from colours or binormals"; return; }
        if (!XF_TEX_GET_INPUT_FORM(ctrl)) in[2] = 1;  // AB11: the third input is one whatever the source
        const std::uint32_t index = texMatrixIndex(v, coord);
        const int rows = XF_TEX_GET_PROJECTION(ctrl) ? 3 : 2;
        float c[3] = {0, 0, 1};
        for (int r = 0; r < rows; ++r)
            c[r] = matrix(index + r, 0) * in[0] + matrix(index + r, 1) * in[1] + matrix(index + r, 2) * in[2] + matrix(index + r, 3) * in[3];
        if (gpu.xf[XfDualTex] & 1) {
            const std::uint32_t post = gpu.xf[XfPostTexGen + coord];
            if (XF_DUALTEX_F_GET_NORMAL_ENABLE(post)) {
                const float length = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
                if (length > 0) for (float& k : c) k /= length;
            }
            const std::uint32_t base = XfPostMatrices / 4 + XF_DUALTEX_F_GET_DUALMATRIX_ADRS(post);
            float p[3];
            for (int r = 0; r < 3; ++r) p[r] = matrix(base + r, 0) * c[0] + matrix(base + r, 1) * c[1] + matrix(base + r, 2) * c[2] + matrix(base + r, 3);
            std::memcpy(c, p, sizeof c);
        }
        // The GPU divides by q for each pixel; here it is divided once per vertex.
        if (std::fabs(c[2] - 1) > 1e-5f && c[2] != 0) {
            st[0] = c[0] / c[2], st[1] = c[1] / c[2];
        } else {
            st[0] = c[0], st[1] = c[1];
        }
    }

    // Channel 0's colour and alpha at a vertex, lit when its control words say (the normal through its normal matrix).
    void channel(const GxVertex& v, std::uint32_t index, const float view[3], std::uint8_t color[4]) {
        if ((gpu.xf[XfNumChans] & 3) == 0) {
            std::memset(color, 0, 4);
            return;
        }
        float normal[3] = {0, 0, 0};
        if (XF_COLOR0CNTRL_F_GET_LIGHTFUNC(gpu.xf[XfColor0Ctrl]) || XF_COLOR0CNTRL_F_GET_LIGHTFUNC(gpu.xf[XfAlpha0Ctrl])) {
            // Normal matrices: three rows of three from XF 0x400, at the position matrix's row.
            const std::uint32_t base = 0x400 + (index & 31) * 3;
            for (int r = 0; r < 3; ++r)
                normal[r] = asFloat(gpu.xf[base + r * 3]) * v.normal[0] + asFloat(gpu.xf[base + r * 3 + 1]) * v.normal[1] +
                            asFloat(gpu.xf[base + r * 3 + 2]) * v.normal[2];
            normalize(normal);
        }
        gxLitChannel(gpu, view, normal, v.color[0], color);
    }

    // Position through its matrix and the projection, then the viewport onto the EFB, then the EFB onto [-1, 1].
    GxDrawVertex vertex(const GxVertex& v, float screen[2]) {
        const std::uint32_t index = gpu.cp[0x50] & 1 ? v.matrixIndex : gpu.xf[XfMatIndexA] & 0x3F;
        float view[3];
        for (int r = 0; r < 3; ++r)
            view[r] = matrix(index + r, 0) * v.position[0] + matrix(index + r, 1) * v.position[1] + matrix(index + r, 2) * v.position[2] + matrix(index + r, 3);
        float p[6];
        for (int k = 0; k < 6; ++k) p[k] = asFloat(gpu.xf[XfProjection + k]);
        const bool orthographic = gpu.xf[XfProjection + 6] != 0;
        const float xc = orthographic ? p[0] * view[0] + p[1] : p[0] * view[0] + p[1] * view[2];
        const float yc = orthographic ? p[2] * view[1] + p[3] : p[2] * view[1] + p[3] * view[2];
        const float zc = p[4] * view[2] + p[5], wc = orthographic ? 1.0f : -view[2];
        float vp[6];
        for (int k = 0; k < 6; ++k) vp[k] = asFloat(gpu.xf[XfViewport + k]);
        // EFB pixel = ndc * scale + offset - 342; depth = ndc_z * zscale + zoffset, over 2^24 - 1.
        const float ex = xc * vp[0] + (vp[3] - 342) * wc, ey = yc * vp[1] + (vp[4] - 342) * wc;
        GxDrawVertex d{};
        d.position[0] = ex / frame.width * 2 - wc;
        d.position[1] = wc - ey / frame.height * 2;
        d.position[2] = (zc * vp[2] + vp[5] * wc) / 16777215.0f - wc;  // citro3d's range: near -1, far 0
        d.position[3] = wc;
        screen[0] = wc != 0 ? ex / wc : 0, screen[1] = wc != 0 ? ey / wc : 0;
        channel(v, index, view, d.color);
        return d;
    }
};
} // namespace

void buildGxDraw(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices,
                 const GxFrame& frame, GxDraw& out) {
    out = GxDraw{};
    readTev(gpu, out);
    readPixel(gpu, out);
    out.program = compileTev(out.material);
    if (kind >= 0xA8) out.unsupported = "lines and points";
    if (out.unsupported) return;

    Transformer transform{gpu, frame, out};
    std::vector<GxDrawVertex> drawn(count);
    std::vector<std::array<float, 2>> screen(count);
    std::vector<bool> behind(count);
    const std::uint32_t size = gpu.vertexSize(format);
    for (std::uint16_t i = 0; i < count; ++i) {
        const GxVertex v = gpu.readVertex(vertices + i * size, format);
        drawn[i] = transform.vertex(v, screen[i].data());
        behind[i] = drawn[i].position[3] <= 0;
        for (unsigned u = 0; u < out.program.unitCount; ++u) transform.texgen(v, out.program.units[u].texCoord, drawn[i].texcoord[u]);
    }
    if (out.unsupported) return;

    // GEN_MODE's reject bits as GXSetCullMode swaps them in: 1 culls what GX_CULL_BACK names, 2 GX_CULL_FRONT.
    const std::uint32_t reject = GEN_MODE_GET_REJECT_EN(gpu.bp[0x00]);
    const auto emit = [&](int a, int b, int c) {
        if (reject && !behind[a] && !behind[b] && !behind[c]) {
            const float area = (screen[b][0] - screen[a][0]) * (screen[c][1] - screen[a][1]) -
                               (screen[b][1] - screen[a][1]) * (screen[c][0] - screen[a][0]);
            if (reject == 3 || (reject == 1 && area < 0) || (reject == 2 && area > 0)) return;
        }
        out.triangles.push_back(drawn[a]), out.triangles.push_back(drawn[b]), out.triangles.push_back(drawn[c]);
    };
    switch (kind) {
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
}
} // namespace smg
