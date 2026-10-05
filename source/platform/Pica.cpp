// What both renderers hand the PICA: textures in its tiled layout, and a GX material's combiner and pixel tests.
#include <smg/Etc1.hpp>
#include <smg/Pica.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace smg {
namespace {
unsigned morton(unsigned x, unsigned y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

GPU_TEXTURE_WRAP_PARAM wrap(unsigned value) {
    return value == 1 ? GPU_REPEAT : value == 2 ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE;
}

unsigned powerOfTwo(unsigned value) {
    unsigned result = 8;
    while (result < value && result < 1024) result <<= 1;
    return result;
}

// The smallest native format that holds the image without visible loss.
GPU_TEXCOLOR chooseFormat(const ModelTexture& texture) {
    bool opaque = true, binary = true;
    for (auto c : texture.image.pixels) {
        const unsigned a = c & 255;
        opaque &= a == 255;
        binary &= a == 0 || a == 255;
    }
    const bool fourBit = texture.format == GxFormat::I4 || texture.format == GxFormat::IA4;
    // CMPR is already four-bit block compression; ETC1 keeps it that size instead of 16 bits.
    if (texture.format == GxFormat::CMPR) return opaque ? GPU_ETC1 : GPU_ETC1A4;
    if (texture.image.grayscale) return opaque ? GPU_L8 : fourBit ? GPU_LA4 : GPU_LA8;
    if (opaque) return GPU_RGB565;
    if (binary) return GPU_RGBA5551;
    return texture.format == GxFormat::RGB5A3 ? GPU_RGBA4 : GPU_RGBA8;
}

// Runs on the loader thread: converts and swizzles into the layout the GPU samples.
} // namespace

GPU_TEXCOLOR tileFormat(const ModelTexture& source) { return chooseFormat(source); }

TiledTexture tile(const ModelTexture& source, int format) {
    TiledTexture out;
    const Image& image = source.image;
    if (!image.width || !image.height) return out;
    out.width = powerOfTwo(image.width);
    out.height = powerOfTwo(image.height);
    out.format = format >= 0 ? static_cast<GPU_TEXCOLOR>(format) : chooseFormat(source);
    out.wrapS = source.wrapS;
    out.wrapT = source.wrapT;
    out.minFilter = source.minFilter;
    out.magFilter = source.magFilter;
    // GX reads v down from the first row; the PICA reads t up from the last row in memory, so rows go in upside down.
    // Nearest-neighbor stretch for the few textures that are not a power of two, by a column map made once: the ARM11
    // divides in software.
    const auto sourceRow = [&](unsigned y) { return image.pixels.data() + std::size_t((out.height - 1 - y) * image.height / out.height) * image.width; };
    std::vector<unsigned> columns(out.width);
    for (unsigned x = 0; x < out.width; ++x) columns[x] = x * image.width / out.width;
    const bool sameWidth = out.width == image.width;
    if (out.format == GPU_ETC1 || out.format == GPU_ETC1A4) {
        Image resized;
        resized.width = out.width;
        resized.height = out.height;
        resized.pixels.resize(std::size_t(out.width) * out.height);
        for (unsigned y = 0; y < out.height; ++y) {
            const std::uint32_t* from = sourceRow(y);
            std::uint32_t* to = resized.pixels.data() + std::size_t(y) * out.width;
            if (sameWidth) std::memcpy(to, from, out.width * sizeof *to);
            else for (unsigned x = 0; x < out.width; ++x) to[x] = from[columns[x]];
        }
        out.bytes = encodeEtc1(resized, out.format == GPU_ETC1A4);
        return out;
    }
    const unsigned bytesPerTexel = out.format == GPU_RGBA8 ? 4 : out.format == GPU_LA4 || out.format == GPU_L8 ? 1 : 2;
    out.bytes.assign(std::size_t(out.width) * out.height * bytesPerTexel, 0);
    auto* bytes = out.bytes.data();
    for (unsigned y = 0; y < out.height; ++y) {
        const std::uint32_t* row = sourceRow(y);
        for (unsigned x = 0; x < out.width; ++x) {
            const std::uint32_t c = row[columns[x]];
            const unsigned r = c >> 24, g = c >> 16 & 255, b = c >> 8 & 255, a = c & 255;
            const unsigned texel = ((y / 8) * (out.width / 8) + x / 8) * 64 + morton(x, y);
            std::uint16_t half = 0;
            switch (out.format) {
            case GPU_RGBA8: std::memcpy(bytes + texel * 4, &c, 4); continue;
            case GPU_RGB565: half = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); break;
            case GPU_RGBA5551: half = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7); break;
            case GPU_RGBA4: half = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4); break;
            case GPU_LA8: half = (r << 8) | a; break;
            case GPU_LA4: bytes[texel] = ((r >> 4) << 4) | (a >> 4); continue;
            default: bytes[texel] = r; continue;
            }
            std::memcpy(bytes + texel * 2, &half, 2);
        }
    }
    return out;
}

bool upload(C3D_Tex& texture, const TiledTexture& tiled) {
    if (!tiled.width || !C3D_TexInit(&texture, tiled.width, tiled.height, tiled.format)) return false;
    std::memcpy(texture.data, tiled.bytes.data(), std::min<std::size_t>(texture.size, tiled.bytes.size()));
    C3D_TexFlush(&texture);
    C3D_TexSetFilter(&texture, tiled.magFilter ? GPU_LINEAR : GPU_NEAREST, tiled.minFilter ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(&texture, wrap(tiled.wrapS), wrap(tiled.wrapT));
    return true;
}

namespace {
GPU_TESTFUNC testFunction(unsigned gx) {
    const GPU_TESTFUNC functions[]{GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS};
    return functions[gx & 7];
}

// citro3d's projection keeps nearer fragments at larger depth values.
GPU_TESTFUNC depthFunction(unsigned gx) {
    const GPU_TESTFUNC functions[]{GPU_NEVER, GPU_GREATER, GPU_EQUAL, GPU_GEQUAL, GPU_LESS, GPU_NOTEQUAL, GPU_LEQUAL, GPU_ALWAYS};
    return functions[gx & 7];
}

u32 packColor(const float* c) {
    const auto byte = [](float v) { return static_cast<u32>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return byte(c[0]) | (byte(c[1]) << 8) | (byte(c[2]) << 16) | (byte(c[3]) << 24);
}

GPU_TEVSRC tevSource(CombineSource source) {
    switch (source) {
    case CombineSource::Primary: return GPU_PRIMARY_COLOR;
    case CombineSource::Texture0: return GPU_TEXTURE0;
    case CombineSource::Texture1: return GPU_TEXTURE1;
    case CombineSource::Texture2: return GPU_TEXTURE2;
    case CombineSource::Constant: return GPU_CONSTANT;
    case CombineSource::Buffer: return GPU_PREVIOUS_BUFFER;
    default: return GPU_PREVIOUS;
    }
}

GPU_TEVOP_RGB rgbOperand(CombineOperand operand) {
    const GPU_TEVOP_RGB operands[]{GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA,
        GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_ONE_MINUS_SRC_R, GPU_TEVOP_RGB_SRC_G,
        GPU_TEVOP_RGB_ONE_MINUS_SRC_G, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_ONE_MINUS_SRC_B};
    return operands[static_cast<int>(operand)];
}

GPU_TEVOP_A alphaOperand(CombineOperand operand) {
    switch (operand) {
    case CombineOperand::OneMinusAlpha: case CombineOperand::OneMinusColor: return GPU_TEVOP_A_ONE_MINUS_SRC_ALPHA;
    case CombineOperand::Red: return GPU_TEVOP_A_SRC_R;
    case CombineOperand::OneMinusRed: return GPU_TEVOP_A_ONE_MINUS_SRC_R;
    case CombineOperand::Green: return GPU_TEVOP_A_SRC_G;
    case CombineOperand::OneMinusGreen: return GPU_TEVOP_A_ONE_MINUS_SRC_G;
    case CombineOperand::Blue: return GPU_TEVOP_A_SRC_B;
    case CombineOperand::OneMinusBlue: return GPU_TEVOP_A_ONE_MINUS_SRC_B;
    default: return GPU_TEVOP_A_SRC_ALPHA;
    }
}

GPU_COMBINEFUNC combineFunction(CombineFunc func) {
    const GPU_COMBINEFUNC functions[]{GPU_REPLACE, GPU_MODULATE, GPU_ADD, GPU_ADD_SIGNED, GPU_INTERPOLATE, GPU_SUBTRACT, GPU_MULTIPLY_ADD};
    return functions[static_cast<int>(func)];
}

GPU_TEVSCALE tevScale(std::uint8_t scale) {
    return scale == 4 ? GPU_TEVSCALE_4 : scale == 2 ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1;
}

} // namespace

PicaPixel preparePixel(const ModelMaterial& m, const CombinerProgram& program) {
    PicaPixel out;
    for (unsigned i = 0; i < CombinerProgram::MaxStages; ++i) {
        auto* env = &out.env[i];
        C3D_TexEnvInit(env);
        if (i >= program.stageCount) continue;
        const auto& s = program.stages[i];
        C3D_TexEnvSrc(env, C3D_RGB, tevSource(s.colorSource[0]), tevSource(s.colorSource[1]), tevSource(s.colorSource[2]));
        C3D_TexEnvSrc(env, C3D_Alpha, tevSource(s.alphaSource[0]), tevSource(s.alphaSource[1]), tevSource(s.alphaSource[2]));
        C3D_TexEnvOpRgb(env, rgbOperand(s.colorOperand[0]), rgbOperand(s.colorOperand[1]), rgbOperand(s.colorOperand[2]));
        C3D_TexEnvOpAlpha(env, alphaOperand(s.alphaOperand[0]), alphaOperand(s.alphaOperand[1]), alphaOperand(s.alphaOperand[2]));
        C3D_TexEnvFunc(env, C3D_RGB, combineFunction(s.colorFunc));
        C3D_TexEnvFunc(env, C3D_Alpha, combineFunction(s.alphaFunc));
        C3D_TexEnvScale(env, C3D_RGB, tevScale(s.colorScale));
        C3D_TexEnvScale(env, C3D_Alpha, tevScale(s.alphaScale));
        C3D_TexEnvColor(env, packColor(s.constant.data()));
    }
    out.bufferColor = packColor(program.bufferColor.data());
    out.bufferUpdate[0] = program.bufferUpdate[0];
    out.bufferUpdate[1] = program.bufferUpdate[1];

    const auto& test = m.alphaTest;
    unsigned compare = test.compare0, reference = test.reference0;
    if ((test.op == 0 && test.compare0 == 7) || (test.op == 1 && test.compare0 == 0)) {
        compare = test.compare1;
        reference = test.reference1;
    }
    if (test.op == 1 && (test.compare0 == 7 || test.compare1 == 7)) compare = 7;
    out.alphaTest = compare != 7;
    out.alphaFunc = testFunction(compare);
    out.alphaReference = reference;
    const GPU_BLENDFACTOR sources[]{GPU_ZERO, GPU_ONE, GPU_DST_COLOR, GPU_ONE_MINUS_DST_COLOR, GPU_SRC_ALPHA,
                                    GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA};
    const GPU_BLENDFACTOR destinations[]{GPU_ZERO, GPU_ONE, GPU_SRC_COLOR, GPU_ONE_MINUS_SRC_COLOR, GPU_SRC_ALPHA,
                                         GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA};
    if (m.blend.type == 1) {
        out.source = sources[m.blend.source & 7];
        out.destination = destinations[m.blend.destination & 7];
    } else if (m.blend.type == 3) {
        out.equation = GPU_BLEND_REVERSE_SUBTRACT;
        out.source = out.destination = GPU_ONE;
    }
    // GX blends alpha as it blends color, subtracting it too (Dolphin's reference does the same).
    out.alphaEquation = out.equation, out.alphaSource = out.source, out.alphaDestination = out.destination;
    out.depthTest = m.depth.enable;
    out.depthFunc = depthFunction(m.depth.function);
    out.depthWrite = m.depth.enable && m.depth.update;
    return out;
}

void PicaPixel::apply() const {
    for (unsigned i = 0; i < CombinerProgram::MaxStages; ++i) *C3D_GetTexEnv(i) = env[i];
    C3D_TexEnvBufColor(bufferColor);
    C3D_TexEnvBufUpdate(C3D_RGB, bufferUpdate[0]);
    C3D_TexEnvBufUpdate(C3D_Alpha, bufferUpdate[1]);
    C3D_AlphaTest(alphaTest, alphaFunc, alphaReference);
    C3D_AlphaBlend(equation, alphaEquation, source, destination, alphaSource, alphaDestination);
    C3D_DepthTest(depthTest, depthTest ? depthFunc : GPU_ALWAYS, depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
}
} // namespace smg
