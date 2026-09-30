#include <smg/SceneRenderer.hpp>
#include <ModelShader.hpp>
#include <smg/Animation.hpp>
#include <smg/Archive.hpp>
#include <smg/Etc1.hpp>
#include <smg/Scene.hpp>
#include <smg/StageCollision.hpp>
#include <smg/StageGravity.hpp>
#include <smg/StageLight.hpp>
#include <Game/Map/LightDataHolder.hpp>
#include <Game/Gravity/GravityInfo.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <Game/Util/StringUtil.hpp>
#include <algorithm>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace smg {
struct SceneRenderer::Material {
    C3D_TexEnv env[CombinerProgram::MaxStages];
    u32 bufferColor = 0;
    int bufferUpdate[2]{};
    int textures[CombinerProgram::MaxUnits]{-1, -1, -1};
    C3D_FVec texgen[CombinerProgram::MaxUnits][3];
    C3D_FVec materialColor, colorSelect, alphaSelect, colorLights, alphaLights;
    GPU_CULLMODE cull = GPU_CULL_BACK_CCW;
    bool alphaTest = false;
    GPU_TESTFUNC alphaFunc = GPU_ALWAYS;
    int alphaReference = 0;
    GPU_BLENDEQUATION equation = GPU_BLEND_ADD;
    GPU_BLENDFACTOR source = GPU_ONE, destination = GPU_ZERO;
    bool depthTest = true, depthWrite = true;
    GPU_TESTFUNC depthFunc = GPU_GEQUAL;
    bool translucent = false;
};

struct SceneRenderer::GpuModel {
    std::string name;
    // A BRK named after the model sets TEV registers; each frame placements use gets its own materials.
    std::unique_ptr<TevRegAnimation> brk;
    std::vector<ModelMaterial> source;
    std::map<int, std::vector<Material>> framed;
    void* vertices = nullptr;
    void* indices = nullptr;
    std::vector<PackedShape> shapes;
    std::vector<C3D_Tex> textures;
    std::vector<Material> materials;
    float center[3]{}, radius = 0;
    GpuModel() = default;
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;
    ~GpuModel() {
        for (auto& texture : textures) if (texture.data) C3D_TexDelete(&texture);
        if (vertices) linearFree(vertices);
        if (indices) linearFree(indices);
    }
};

namespace {
constexpr float Near = 30.0f, Far = 400000.0f, FieldOfView = 55.0f;
constexpr std::size_t ArchiveBudget = 32u * 1024 * 1024, ModelBudget = 24u * 1024 * 1024;

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

struct TiledTexture {
    unsigned width = 0, height = 0, wrapS = 0, wrapT = 0, minFilter = 1, magFilter = 1;
    GPU_TEXCOLOR format = GPU_RGBA8;
    std::vector<std::uint8_t> bytes;
};

// Runs on the loader thread: converts and swizzles into the layout the GPU samples.
TiledTexture tile(const ModelTexture& source) {
    TiledTexture out;
    const Image& image = source.image;
    if (!image.width || !image.height) return out;
    out.width = powerOfTwo(image.width);
    out.height = powerOfTwo(image.height);
    out.format = chooseFormat(source);
    out.wrapS = source.wrapS;
    out.wrapT = source.wrapT;
    out.minFilter = source.minFilter;
    out.magFilter = source.magFilter;
    // GX reads v down from the first row; the PICA reads t up from the last row in memory, so rows go in upside down.
    const auto sourceRow = [&](unsigned y) { return std::size_t((out.height - 1 - y) * image.height / out.height) * image.width; };
    if (out.format == GPU_ETC1 || out.format == GPU_ETC1A4) {
        Image resized;
        resized.width = out.width;
        resized.height = out.height;
        for (unsigned y = 0; y < out.height; ++y)
            for (unsigned x = 0; x < out.width; ++x)
                resized.pixels.push_back(image.pixels[sourceRow(y) + x * image.width / out.width]);
        out.bytes = encodeEtc1(resized, out.format == GPU_ETC1A4);
        return out;
    }
    const unsigned bytesPerTexel = out.format == GPU_RGBA8 ? 4 : out.format == GPU_LA4 || out.format == GPU_L8 ? 1 : 2;
    out.bytes.assign(std::size_t(out.width) * out.height * bytesPerTexel, 0);
    auto* bytes = out.bytes.data();
    for (unsigned y = 0; y < out.height; ++y) {
        for (unsigned x = 0; x < out.width; ++x) {
            // Nearest-neighbour stretch for the few textures that are not a power of two.
            const std::uint32_t c = image.pixels[sourceRow(y) + x * image.width / out.width];
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

SceneRenderer::Material prepare(const ModelMaterial& m) {
    SceneRenderer::Material out;
    const auto program = compileTev(m);
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

    for (unsigned unit = 0; unit < CombinerProgram::MaxUnits; ++unit) {
        float rows[2][3]{{1, 0, 0}, {0, 1, 0}}, select[3]{1, 0, 0};
        if (unit < program.unitCount) {
            out.textures[unit] = program.units[unit].texture;
            const auto& gen = m.texGens[program.units[unit].texCoord];
            if (gen.source == 5) { select[0] = 0; select[1] = 1; }
            else if (gen.source <= 1) { select[0] = 0; select[2] = 1; } // position and normal: environment maps
            const unsigned matrix = (gen.matrix - 30) / 3;
            if (gen.matrix >= 30 && gen.matrix < 60 && matrix < 10 && m.hasTexMatrix[matrix]) {
                const auto& uv = m.texMatrices[matrix].uv;
                for (int c = 0; c < 3; ++c) { rows[0][c] = uv[c]; rows[1][c] = uv[3 + c]; }
            }
        }
        out.texgen[unit][0] = FVec4_New(rows[0][0], rows[0][1], rows[0][2], 0);
        out.texgen[unit][1] = FVec4_New(rows[1][0], rows[1][1], rows[1][2], 0);
        out.texgen[unit][2] = FVec4_New(select[0], select[1], select[2], 0);
    }

    const auto& color = m.channels[0];
    const auto& alpha = m.channels[1];
    out.materialColor = FVec4_New(m.materialColor[0][0], m.materialColor[0][1], m.materialColor[0][2], m.materialColor[0][3]);
    out.colorSelect = FVec4_New(color.materialSource == 1, color.ambientSource == 1, m.channelCount && color.lighting,
                                alpha.materialSource == 1);
    out.alphaSelect = FVec4_New(alpha.ambientSource == 1, m.channelCount && alpha.lighting, 0, 0);
    // Lights 0 to 2 are the director's; point lights (4) come with actors. GX_DF_CLAMP is 2, GX_DF_SIGN 1.
    const auto lights = [](const ColorChannel& c) {
        return FVec4_New(c.lightMask & 1, (c.lightMask >> 1) & 1, (c.lightMask >> 2) & 1, c.diffuse != 1);
    };
    out.colorLights = lights(color);
    out.alphaLights = lights(alpha);

    // GX treats clockwise triangles as front faces.
    out.cull = m.cullMode == 0 || m.cullMode == 3 ? GPU_CULL_NONE : m.cullMode == 2 ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;
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
    out.depthTest = m.depth.enable;
    out.depthFunc = depthFunction(m.depth.function);
    out.depthWrite = m.depth.enable && m.depth.update;
    out.translucent = m.mode == 4 || m.blend.type == 1 || m.blend.type == 3;
    return out;
}

void setMatrix(C3D_Mtx& out, const Transform& t) {
    Mtx_Identity(&out);
    for (int row = 0; row < 3; ++row) {
        out.r[row].x = t[row * 4];
        out.r[row].y = t[row * 4 + 1];
        out.r[row].z = t[row * 4 + 2];
        out.r[row].w = t[row * 4 + 3];
    }
}

// "Disk in DiskGardenZone, pull 0.00 -1.00 0.00": the gravity the original manager picks at a point.
std::string describeGravity(const NativeScene& scene, const Stage& stage, const float at[3]) {
    TVec3f pull;
    GravityInfo info;
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    if (!manager->calcTotalGravityVector(&pull, &info, TVec3f(at[0], at[1], at[2]), GRAVITY_TYPE_NORMAL, 0)) return "none";
    std::string kind = "?", zone = "?";
    for (const auto& placed : scene.placedGravities()) {
        if (placed.gravity != info.mGravityInstance) continue;
        kind = placed.name, zone = stage.zones[placed.zone].name;
    }
    if (kind.rfind("Global", 0) == 0) kind.erase(0, 6);
    if (const auto word = kind.find("Gravity"); word != std::string::npos) kind.erase(word, 7);
    char text[128];
    // Adding zero turns -0.00 into 0.00.
    std::snprintf(text, sizeof text, "%s in %s, pull %.2f %.2f %.2f", kind.c_str(), zone.c_str(), pull.x + 0.0f,
                  pull.y + 0.0f, pull.z + 0.0f);
    return text;
}

// "lands on DiskGardenPlanet 13.9 below, Normal floor": the pull followed to the first collision face.
std::string describeLanding(const StageCollision& collision, const Stage& stage, const float at[3]) {
    TVec3f pull;
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    if (!manager->calcTotalGravityVector(&pull, nullptr, TVec3f(at[0], at[1], at[2]), GRAVITY_TYPE_NORMAL, 0)) return "";
    StageCollision::Hit hit;
    if (!collision.land(TVec3f(at[0], at[1], at[2]), pull, hit)) return "falls onto no collision";
    char text[128];
    // A start can sit a little inside the surface; then the ground is above it.
    std::snprintf(text, sizeof text, "lands on %s %.1f %s, %s floor", stage.objects[hit.object].name.c_str(), std::fabs(hit.distance),
                  hit.distance < 0 ? "above" : "below", hit.floor);
    return text;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && MR::isEqualStringCase(text.c_str() + text.size() - suffix.size(), suffix.c_str());
}
} // namespace

// Mario's model with its skin kept: the CPU skins one vertex buffer while the GPU draws from the other.
struct SceneRenderer::Figure {
    Model model;
    std::vector<std::uint32_t> origin; // the model vertex behind each packed one
    JointAnimation wait;
    J3DFrameCtrl frames;
    std::vector<JointTransform> pose;
    std::vector<std::array<float, 12>> world, draws;
    std::unique_ptr<GpuModel> gpu;
    void* buffers[2]{};
    int drawing = 0;
    float sinceStep = 0;
    std::string where;
    Figure() = default;
    Figure(const Figure&) = delete;
    Figure& operator=(const Figure&) = delete;
    ~Figure() {
        if (gpu) gpu->vertices = nullptr; // one of the two buffers, freed here
        for (void* buffer : buffers) if (buffer) linearFree(buffer);
    }
};

struct SceneRenderer::Prepared {
    enum class Result { Ready, Missing, Skinned, Failed, Figure } result = Result::Failed;
    std::string name, error;
    std::unique_ptr<Figure> figure;
    std::unique_ptr<StageCollision::Model> collision; // read from the same archive, whatever the model is
    std::string collisionError;
    std::unique_ptr<TevRegAnimation> brk;
    std::vector<ModelMaterial> source;
    PackedMesh mesh;
    std::vector<TiledTexture> textures;
    std::vector<Material> materials;
};

namespace {
// Everything a model needs short of GPU memory, so it can run off the main thread.
std::unique_ptr<SceneRenderer::Prepared> prepareModel(const std::string& root, const std::string& name) {
    auto out = std::make_unique<SceneRenderer::Prepared>();
    out->name = name;
    std::vector<std::uint8_t> storage;
    std::vector<ArchiveEntry> entries;
    std::string error;
    out->result = SceneRenderer::Prepared::Result::Missing;
    if (!readArchive((root + "/ObjectData/" + name + ".arc").c_str(), storage, entries, ArchiveBudget, error)) return out;
    out->collision = StageCollision::readModel(name, storage.data(), entries, out->collisionError);
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const ArchiveEntry& e) {
        return endsWith(e.path, name + ".bdl") || endsWith(e.path, name + ".bmd");
    });
    if (entry == entries.end()) return out;
    Model model;
    out->result = SceneRenderer::Prepared::Result::Failed;
    if (!loadModel({storage.data() + entry->offset, entry->size}, model, ModelBudget, error)) return out;
    // MapObjActor::init's tryStartAllAnim plays the BRK named after the model.
    const auto brk = std::find_if(entries.begin(), entries.end(), [&](const ArchiveEntry& e) { return endsWith(e.path, name + ".brk"); });
    if (brk != entries.end()) {
        auto animation = std::make_unique<TevRegAnimation>();
        if (animation->load({storage.data() + brk->offset, brk->size}, error)) {
            out->brk = std::move(animation);
            out->source = model.materials;
        }
    }
    std::vector<std::uint8_t>().swap(storage);
    out->result = SceneRenderer::Prepared::Result::Skinned;
    if (model.skinned) return out;
    out->mesh = packMesh(model);
    for (const auto& texture : model.textures) out->textures.push_back(tile(texture));
    for (const auto& material : model.materials) out->materials.push_back(prepare(material));
    out->result = SceneRenderer::Prepared::Result::Ready;
    return out;
}

// MarioActor::initDrawAndModel's "Mario" with "MarioAnime", and the wait animation he stands in.
std::unique_ptr<SceneRenderer::Prepared> prepareFigure(const std::string& root) {
    auto out = std::make_unique<SceneRenderer::Prepared>();
    out->name = "Mario";
    auto figure = std::make_unique<SceneRenderer::Figure>();
    std::vector<std::uint8_t> storage;
    std::vector<ArchiveEntry> entries;
    // The whole file name, so "wait.bck" does not find "hipwait.bck".
    const auto find = [&](const std::string& file) {
        return std::find_if(entries.begin(), entries.end(), [&](const ArchiveEntry& e) {
            return MR::isEqualStringCase(e.path.c_str(), file.c_str()) || endsWith(e.path, "/" + file);
        });
    };
    auto& error = out->error;
    out->result = SceneRenderer::Prepared::Result::Missing;
    if (!readArchive((root + "/ObjectData/Mario.arc").c_str(), storage, entries, ArchiveBudget, error)) return out;
    const auto model = find("mario.bdl");
    if (model == entries.end()) { error = "Mario.arc has no mario.bdl"; return out; }
    out->result = SceneRenderer::Prepared::Result::Failed;
    if (!loadModel({storage.data() + model->offset, model->size}, figure->model, ModelBudget, error, true)) return out;
    out->result = SceneRenderer::Prepared::Result::Missing;
    if (!readArchive((root + "/ObjectData/MarioAnime.arc").c_str(), storage, entries, ArchiveBudget, error)) return out;
    const auto wait = find("wait.bck");
    if (wait == entries.end()) { error = "MarioAnime.arc has no wait.bck"; return out; }
    out->result = SceneRenderer::Prepared::Result::Failed;
    if (!figure->wait.load({storage.data() + wait->offset, wait->size}, error)) return out;
    std::vector<std::uint8_t>().swap(storage);

    const Model& mario = figure->model;
    out->mesh = packMesh(mario, &figure->origin);
    for (const auto& texture : mario.textures) out->textures.push_back(tile(texture));
    for (const auto& material : mario.materials) out->materials.push_back(prepare(material));
    // Joints the animation leaves alone keep their bind pose.
    for (const auto& joint : mario.joints) {
        JointTransform t;
        std::copy(joint.scale, joint.scale + 3, t.scale);
        std::copy(joint.rotation, joint.rotation + 3, t.rotation);
        std::copy(joint.translation, joint.translation + 3, t.translation);
        figure->pose.push_back(t);
    }
    // As J3D's animation controls start a BCK: its length, its own play mode, one frame per game frame.
    figure->frames.init(figure->wait.key().mFrameMax);
    figure->frames.setAttribute(figure->wait.key().mAttribute);
    out->figure = std::move(figure);
    out->result = SceneRenderer::Prepared::Result::Figure;
    return out;
}
} // namespace

SceneRenderer::SceneRenderer() { LightLock_Init(&lock_); }
SceneRenderer::~SceneRenderer() { shutdown(); }

void SceneRenderer::workerMain(void* self) { static_cast<SceneRenderer*>(self)->work(); }

void SceneRenderer::work() {
    for (std::size_t i = 0; i < names_.size(); ++i) {
        // Hold at most two decoded models, so the loader cannot run far ahead of the uploads.
        for (;;) {
            if (stopping_) return;
            LightLock_Lock(&lock_);
            const std::size_t queued = finished_.size();
            LightLock_Unlock(&lock_);
            if (queued < 2) break;
            svcSleepThread(2000000);
        }
        auto prepared = i < placedNames_ ? prepareModel(root_, names_[i]) : prepareFigure(root_);
        LightLock_Lock(&lock_);
        finished_.push_back(std::move(prepared));
        LightLock_Unlock(&lock_);
    }
}

void SceneRenderer::stopWorker() {
    stopping_ = true;
    if (worker_) {
        threadJoin(worker_, U64_MAX);
        threadFree(worker_);
        worker_ = nullptr;
    }
    stopping_ = false;
    LightLock_Lock(&lock_);
    finished_.clear();
    LightLock_Unlock(&lock_);
}

bool SceneRenderer::init() {
    shader_ = DVLB_ParseFile(reinterpret_cast<u32*>(modelShader), sizeof(modelShader));
    if (!shader_) { status_ = "Cannot parse native model shader"; return false; }
    shaderProgramInit(&program_);
    programReady_ = R_SUCCEEDED(shaderProgramSetVsh(&program_, &shader_->DVLE[0]));
    if (!programReady_) { status_ = "Cannot create model shader"; return false; }
    const auto uniform = [&](const char* name) { return shaderInstanceGetUniformLocation(program_.vertexShader, name); };
    projection_ = uniform("projection");
    modelView_ = uniform("modelView");
    materialColor_ = uniform("materialColor");
    lightAmbient_ = uniform("lightAmbient");
    light0Pos_ = uniform("light0Pos");
    light0Color_ = uniform("light0Color");
    light1Pos_ = uniform("light1Pos");
    light1Color_ = uniform("light1Color");
    light2Alpha_ = uniform("light2Alpha");
    colorSelect_ = uniform("colorSelect");
    alphaSelect_ = uniform("alphaSelect");
    colorLights_ = uniform("colorLights");
    alphaLights_ = uniform("alphaLights");
    texgen_[0] = uniform("texgen0");
    texgen_[1] = uniform("texgen1");
    texgen_[2] = uniform("texgen2");
    for (int location : {projection_, modelView_, materialColor_, lightAmbient_, light0Pos_, light0Color_, light1Pos_,
                         light1Color_, light2Alpha_, colorSelect_, alphaSelect_, colorLights_, alphaLights_, texgen_[0],
                         texgen_[1], texgen_[2]}) {
        if (location < 0) { status_ = "Missing model shader uniforms"; programReady_ = false; return false; }
    }
    return true;
}

void SceneRenderer::shutdown() {
    stopWorker();
    collision_.reset();
    gravity_.reset();
    instances_.clear();
    figure_.reset();
    models_.clear();
    names_.clear();
    consumed_ = 0;
    if (programReady_) shaderProgramFree(&program_);
    programReady_ = false;
    if (shader_) DVLB_Free(shader_);
    shader_ = nullptr;
}

void SceneRenderer::loadStage(const std::string& root, const std::string& galaxy, int scenario) {
    stopWorker();
    instances_.clear();
    figure_.reset();
    figureState_.clear();
    models_.clear();
    names_.clear();
    consumed_ = skinned_ = missing_ = failed_ = 0;
    root_ = root;
    // The collision's parts read the scene's codes, so they go before the scene does.
    collision_.reset();
    gravity_.reset();
    startLanding_.clear();
    title_ = galaxy + " / scenario " + std::to_string(scenario);
    std::string error;
    if (!smg::loadStage(root, galaxy, scenario, stage_, error)) { status_ = error; return; }
    for (const auto& object : stage_.objects)
        if (std::find(names_.begin(), names_.end(), object.name) == names_.end()) names_.push_back(object.name);
    placedNames_ = names_.size();
    gravity_ = std::make_unique<NativeScene>();
    if (smg::placeStageGravity(root, stage_, *gravity_, error)) {
        startGravity_ = stage_.entrance ? describeGravity(*gravity_, stage_, stage_.start.data()) : "no MarioNo 0 entrance";
        // Each model's collision arrives with it from the loader thread.
        collision_ = std::make_unique<StageCollision>();
        lightSets_.clear();
        lightError_ = smg::loadStageLights(root, *gravity_, error) ? "" : "lights unavailable: " + error;
        lightSet(gravity_->defaultAreaLight());
        // Mario goes last, when every collision part he could stand on is in.
        if (stage_.entrance) names_.push_back("Mario");
    } else {
        startGravity_ = "unavailable: " + error;
        gravity_.reset();
        lightSets_.clear();
        lightSet(nullptr);
    }
    view_ = 0;
    resetCamera();
    status_ = std::to_string(stage_.objects.size()) + " placements, " + std::to_string(names_.size()) + " models to load";
    // Just below the main thread, so decoding fills the time the main thread spends waiting on the GPU.
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    worker_ = threadCreate(workerMain, this, 256 * 1024, std::min<s32>(priority + 1, 0x3f), -2, false);
    if (!worker_) {
        // Nothing will arrive, so stop waiting and keep the reason on screen.
        names_.clear();
        status_ = summary_ = "Cannot start the model loader";
    }
}

// Placed models draw in the map and planet buffers, which take the area light's planet set; the player,
// enemy and NPC sets come with actors. Without light data every channel sees plain white.
int SceneRenderer::lightSet(const AreaLightInfo* area, bool player) {
    for (std::size_t i = 0; i < lightSets_.size(); ++i)
        if (lightSets_[i].area == area && lightSets_[i].player == player) return static_cast<int>(i);
    LightSet set{area, player, {}, {1, 1, 1, 1}, 0};
    if (area) {
        const auto unit = [](const GXColor& c, float* out) {
            out[0] = c.r / 255.0f, out[1] = c.g / 255.0f, out[2] = c.b / 255.0f, out[3] = c.a / 255.0f;
        };
        // ActorLightCtrl::getTargetActorLight: the player's control asks for type 0.
        const ActorLightInfo& planet = player ? area->mPlayerLight : area->mPlanetLight;
        const LightInfo* infos[2]{&planet.mInfo0, &planet.mInfo1};
        for (int i = 0; i < 2; ++i) {
            unit(infos[i]->mColor, set.lights[i].color);
            set.lights[i].position[0] = infos[i]->mPos.x, set.lights[i].position[1] = infos[i]->mPos.y;
            set.lights[i].position[2] = infos[i]->mPos.z;
            set.lights[i].followCamera = infos[i]->mIsFollowCamera;
        }
        unit(planet.mColor, set.ambient);
        set.alpha = planet.mAlpha2 / 255.0f;
    }
    lightSets_.push_back(set);
    return static_cast<int>(lightSets_.size()) - 1;
}

// LightFunction::loadActorLightInfo: positions go into view space unless they follow the camera already;
// the third light, at the camera, lights alpha only.
void SceneRenderer::loadLightSet(const LightSet& set, const C3D_Mtx& view) {
    for (int i = 0; i < 2; ++i) {
        const auto& light = set.lights[i];
        float at[3]{light.position[0], light.position[1], light.position[2]};
        if (!light.followCamera) {
            for (int row = 0; row < 3; ++row)
                at[row] = view.r[row].x * light.position[0] + view.r[row].y * light.position[1] + view.r[row].z * light.position[2] + view.r[row].w;
        }
        C3D_FVUnifSet(GPU_VERTEX_SHADER, i ? light1Pos_ : light0Pos_, at[0], at[1], at[2], 1);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, i ? light1Color_ : light0Color_, light.color[0], light.color[1], light.color[2], light.color[3]);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, light2Alpha_, set.alpha, 0, 0, 0);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, lightAmbient_, set.ambient[0], set.ambient[1], set.ambient[2], set.ambient[3]);
}

void SceneRenderer::dropProbe() {
    probeFalling_ = false;
    probeTrail_.clear();
    probeState_.clear();
    if (!gravity_ || !collision_ || !stage_.entrance || loading()) return;
    TVec3f pull;
    const TVec3f start(stage_.start[0], stage_.start[1], stage_.start[2]);
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    if (!manager->calcTotalGravityVector(&pull, nullptr, start, GRAVITY_TYPE_NORMAL, 0)) return;
    // 800 units up the start's own pull, where the start view can see it fall.
    for (int i = 0; i < 3; ++i) probe_[i] = stage_.start[i] - (&pull.x)[i] * 800, probeVelocity_[i] = 0;
    probeFallen_ = probeSinceTrail_ = 0;
    probeFalling_ = true;
    probeState_ = "probe";
}

void SceneRenderer::stepProbe(float seconds) {
    // A fixed pull and top speed for a diagnostic marker; Mario's own physics are not what this shows.
    constexpr float Acceleration = 500, TopSpeed = 4000, Step = 1 / 60.0f, GiveUp = 30000;
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    for (float left = seconds; probeFalling_ && left > 0; left -= Step) {
        const float dt = std::min(left, Step);
        const TVec3f at(probe_[0], probe_[1], probe_[2]);
        TVec3f pull;
        if (!manager->calcTotalGravityVector(&pull, nullptr, at, GRAVITY_TYPE_NORMAL, 0)) {
            probeState_ = "probe: no gravity here";
            return;
        }
        float speed = 0;
        for (int i = 0; i < 3; ++i) {
            probeVelocity_[i] += (&pull.x)[i] * Acceleration * dt;
            speed += probeVelocity_[i] * probeVelocity_[i];
        }
        speed = std::sqrt(speed);
        if (speed > TopSpeed) for (float& v : probeVelocity_) v *= TopSpeed / speed;
        const TVec3f step(probeVelocity_[0] * dt, probeVelocity_[1] * dt, probeVelocity_[2] * dt);
        StageCollision::Hit hit;
        if (collision_->strikeLine(at, step, hit)) {
            for (int i = 0; i < 3; ++i) probe_[i] = (&hit.position.x)[i] + (&hit.normal.x)[i] * 2;
            probeFallen_ += hit.distance;
            probeFalling_ = false;
            char text[128];
            std::snprintf(text, sizeof text, "probe: on %s, %s floor, fell %.0f", stage_.objects[hit.object].name.c_str(), hit.floor,
                          probeFallen_);
            probeState_ = text;
        } else {
            for (int i = 0; i < 3; ++i) probe_[i] += (&step.x)[i];
            probeFallen_ += step.length();
            char text[64];
            std::snprintf(text, sizeof text, "probe: falling, %.0f so far", probeFallen_);
            probeState_ = text;
            if (probeFallen_ > GiveUp) {
                probeFalling_ = false;
                probeState_ = "probe: fell 30000 without meeting collision";
            }
        }
        probeSinceTrail_ += dt;
        if (probeSinceTrail_ >= 0.05f) {
            probeSinceTrail_ = 0;
            probeTrail_.push_back({probe_[0], probe_[1], probe_[2]});
        }
    }
}

void SceneRenderer::resetCamera() {
    // Behind and above the first start point looking the way it faces; close in front looking back at it; level
    // with a standing figure's head; or close behind it. The near views turn a little so the pointer misses it.
    struct View { float away, above, turn, pitch; };
    constexpr View views[]{{-2400, 900, 0, -0.35f}, {500, 330, 3.14159265f - 0.25f, -0.45f}, {210, 118, 3.14159265f - 0.3f, 0.0f},
                           {-330, 200, 0.28f, -0.3f}};
    const View& view = views[view_];
    const float facing = stage_.startRotation[1] * 3.14159265f / 180.0f;
    yaw_ = facing + view.turn;
    pitch_ = view.pitch;
    position_[0] = stage_.start[0] + std::sin(facing) * view.away;
    position_[1] = stage_.start[1] + view.above;
    position_[2] = stage_.start[2] + std::cos(facing) * view.away;
}

namespace {
// The frame a placement shows its BRK at: EarthenPipe stops at its color, Obj_arg7 (0 when unset); other map
// parts start theirs at 0 and play it, which waits for animation playback.
int brkFrame(const StageObject& object) {
    if (object.name == "EarthenPipe" || object.name == "EarthenPipeInWater") return object.args[7] < 0 ? 0 : object.args[7];
    return 0;
}
} // namespace

const std::vector<SceneRenderer::Material>& SceneRenderer::framedMaterials(GpuModel& model, int frame) {
    auto found = model.framed.find(frame);
    if (found != model.framed.end()) return found->second;
    std::vector<ModelMaterial> materials = model.source;
    model.brk->apply(static_cast<float>(frame), materials);
    std::vector<Material> prepared;
    for (const auto& material : materials) prepared.push_back(prepare(material));
    return model.framed.emplace(frame, std::move(prepared)).first->second;
}

std::unique_ptr<SceneRenderer::GpuModel> SceneRenderer::uploadModel(Prepared& prepared) {
    auto gpu = std::make_unique<GpuModel>();
    gpu->name = prepared.name;
    const auto& mesh = prepared.mesh;
    gpu->vertices = linearAlloc(mesh.vertices.size() * sizeof(PackedVertex));
    gpu->indices = linearAlloc(mesh.indices.size() * sizeof(std::uint16_t));
    if (!gpu->vertices || !gpu->indices) { ++failed_; status_ = "Out of linear memory at " + prepared.name; return nullptr; }
    std::memcpy(gpu->vertices, mesh.vertices.data(), mesh.vertices.size() * sizeof(PackedVertex));
    std::memcpy(gpu->indices, mesh.indices.data(), mesh.indices.size() * sizeof(std::uint16_t));
    GSPGPU_FlushDataCache(gpu->vertices, mesh.vertices.size() * sizeof(PackedVertex));
    GSPGPU_FlushDataCache(gpu->indices, mesh.indices.size() * sizeof(std::uint16_t));
    gpu->shapes = mesh.shapes;
    std::copy(mesh.center, mesh.center + 3, gpu->center);
    gpu->radius = mesh.radius;
    gpu->textures.resize(prepared.textures.size());
    for (std::size_t i = 0; i < prepared.textures.size(); ++i) {
        if (!upload(gpu->textures[i], prepared.textures[i])) {
            ++failed_;
            status_ = "Cannot upload a texture of " + prepared.name;
            return nullptr;
        }
    }
    gpu->materials = std::move(prepared.materials);
    gpu->brk = std::move(prepared.brk);
    gpu->source = std::move(prepared.source);
    return gpu;
}

void SceneRenderer::consume(Prepared& prepared) {
    if (!prepared.collisionError.empty()) startLanding_ = "collision unavailable: " + prepared.collisionError;
    if (prepared.collision && collision_) collision_->addParts(stage_, std::move(prepared.collision));
    if (prepared.result == Prepared::Result::Figure) { placeFigure(prepared); return; }
    if (!prepared.error.empty()) figureState_ = prepared.name + ": " + prepared.error;
    if (prepared.result == Prepared::Result::Missing) { ++missing_; return; }
    if (prepared.result == Prepared::Result::Skinned) { ++skinned_; return; }
    if (prepared.result != Prepared::Result::Ready) { ++failed_; return; }
    auto gpu = uploadModel(prepared);
    if (!gpu) return;

    const bool sky = prepared.name.size() > 2 && prepared.name[0] == 'V' && prepared.name[1] == 'R';
    for (const auto& object : stage_.objects) {
        if (object.name != prepared.name) continue;
        // ActorLightCtrl::init: the light area around the placement, else the stage's default.
        const int light = gravity_ ? lightSet(gravity_->areaLightAt(TVec3f(object.transform[3], object.transform[7], object.transform[11]))) : 0;
        Instance instance{gpu.get(), {}, {}, 0, sky, light, nullptr};
        if (gpu->brk) instance.materials = &framedMaterials(*gpu, brkFrame(object));
        setMatrix(instance.transform, object.transform);
        const auto& t = object.transform;
        float scale = 0;
        for (int row = 0; row < 3; ++row) {
            instance.center[row] = t[row * 4] * gpu->center[0] + t[row * 4 + 1] * gpu->center[1] +
                                   t[row * 4 + 2] * gpu->center[2] + t[row * 4 + 3];
            scale = std::max(scale, std::sqrt(t[row] * t[row] + t[4 + row] * t[4 + row] + t[8 + row] * t[8 + row]));
        }
        instance.radius = gpu->radius * scale;
        instances_.push_back(instance);
    }
    models_.push_back(std::move(gpu));
}

void SceneRenderer::placeFigure(Prepared& prepared) {
    auto gpu = uploadModel(prepared);
    if (!gpu) { figureState_ = "Mario: " + status_; return; }
    auto figure = std::move(prepared.figure);
    const std::size_t bytes = prepared.mesh.vertices.size() * sizeof(PackedVertex);
    figure->buffers[0] = gpu->vertices;
    figure->buffers[1] = linearAlloc(bytes);
    figure->gpu = std::move(gpu);
    if (!figure->buffers[1]) { ++failed_; figureState_ = "Mario: out of linear memory"; return; }

    // Stood where his start's pull meets the collision, upright against that pull, facing the start's way.
    TVec3f pull;
    const TVec3f start(stage_.start[0], stage_.start[1], stage_.start[2]);
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    if (!manager->calcTotalGravityVector(&pull, nullptr, start, GRAVITY_TYPE_NORMAL, 0)) {
        figureState_ = "Mario: no gravity at his start";
        return;
    }
    float at[3]{start.x, start.y, start.z};
    StageCollision::Hit hit;
    if (collision_ && collision_->land(start, pull, hit)) {
        at[0] = hit.position.x, at[1] = hit.position.y, at[2] = hit.position.z;
        figure->where = "on " + stage_.objects[hit.object].name;
    } else {
        figure->where = "at his start, over no collision";
    }
    const float unit[3]{1, 1, 1};
    const Transform facing = placementMatrix(stage_.start.data(), stage_.startRotation.data(), unit);
    TVec3f up(-pull.x, -pull.y, -pull.z), front(facing[2], facing[6], facing[10]), side;
    up.normalize();
    side.cross(up, front);
    if (side.squared() < 1e-6f) side.cross(up, TVec3f(facing[0], facing[4], facing[8]));
    side.normalize();
    front.cross(side, up);
    Transform placed{side.x, up.x, front.x, at[0], side.y, up.y, front.y, at[1], side.z, up.z, front.z, at[2]};

    GpuModel& model = *figure->gpu;
    const int light = gravity_ ? lightSet(gravity_->areaLightAt(TVec3f(at[0], at[1], at[2])), true) : 0;
    Instance instance{&model, {}, {}, 0, false, light, nullptr};
    setMatrix(instance.transform, placed);
    for (int row = 0; row < 3; ++row)
        instance.center[row] = placed[row * 4] * model.center[0] + placed[row * 4 + 1] * model.center[1] +
                               placed[row * 4 + 2] * model.center[2] + placed[row * 4 + 3];
    instance.radius = model.radius;
    instances_.push_back(instance);
    figure_ = std::move(figure);
    animateFigure(0);
}

void SceneRenderer::animateFigure(float seconds) {
    Figure& figure = *figure_;
    constexpr float Step = 1 / 60.0f;
    figure.sinceStep += seconds;
    for (int steps = 0; figure.sinceStep >= Step; figure.sinceStep -= Step) {
        figure.frames.update();
        // After a long stall, catch up by a few frames rather than all of them.
        if (++steps == 4) figure.sinceStep = 0;
    }
    figure.wait.pose(figure.frames.getFrame(), figure.pose);
    poseSkeleton(figure.model, figure.pose, figure.world);
    skinMatrices(figure.model, figure.world, figure.draws);
    // The GPU may still be drawing last frame's buffer; the other one finished before this frame began.
    figure.drawing ^= 1;
    auto* out = static_cast<PackedVertex*>(figure.buffers[figure.drawing]);
    for (std::size_t i = 0; i < figure.origin.size(); ++i) out[i] = packVertex(skinVertex(figure.model, figure.draws, figure.origin[i]));
    GSPGPU_FlushDataCache(out, figure.origin.size() * sizeof(PackedVertex));
    figure.gpu->vertices = out;
    char text[128];
    std::snprintf(text, sizeof text, "Mario: wait.bck frame %.0f of %d, %s", figure.frames.getFrame(), figure.wait.key().mFrameMax,
                  figure.where.c_str());
    figureState_ = text;
}

void SceneRenderer::update(const InputFrame& input, float seconds) {
    if (loading()) {
        std::unique_ptr<Prepared> next;
        LightLock_Lock(&lock_);
        if (!finished_.empty()) {
            next = std::move(finished_.front());
            finished_.erase(finished_.begin());
        }
        LightLock_Unlock(&lock_);
        if (next) {
            consume(*next);
            ++consumed_;
        }
        status_ = "Loading " + std::to_string(consumed_) + "/" + std::to_string(names_.size()) + "  " +
                  (consumed_ < names_.size() ? names_[consumed_] : std::string("done"));
        summary_ = "loading " + std::to_string(consumed_) + " of " + std::to_string(names_.size()) + " models";
        if (!loading()) {
            char memory[48];
            std::snprintf(memory, sizeof(memory), "; %.1f MB linear free", linearSpaceFree() / (1024.0 * 1024.0));
            // A failure mid-stream is overwritten by later progress lines, so count it here.
            const std::string failed = failed_ ? ", " + std::to_string(failed_) + " failed" : "";
            status_ = std::to_string(models_.size()) + " models, " + std::to_string(instances_.size()) + " placed; skipped " +
                      std::to_string(skinned_) + " animated" + failed + memory;
            if (!lightError_.empty()) status_ = lightError_;
            std::snprintf(memory, sizeof(memory), "%zu models, %.1f MB free", models_.size(), linearSpaceFree() / (1024.0 * 1024.0));
            summary_ = memory + failed;
            if (collision_ && stage_.entrance && startLanding_.empty()) {
                startLanding_ = describeLanding(*collision_, stage_, stage_.start.data());
                char parts[32];
                std::snprintf(parts, sizeof parts, " (%zu parts)", collision_->parts());
                startLanding_ += parts;
            }
            dropProbe();
        }
    }
    if (input.camera.pressed) {
        view_ = (view_ + 1) % 4;
        resetCamera();
        dropProbe();
    }
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 0.25f) return;
    yaw_ -= (input.cameraX * 1.6f + (static_cast<int>(input.dpadRight.held) - input.dpadLeft.held)) * seconds;
    pitch_ += (input.cameraY * 1.2f + (static_cast<int>(input.dpadUp.held) - input.dpadDown.held) * 0.8f) * seconds;
    pitch_ = std::clamp(pitch_, -1.5f, 1.5f);
    const float speed = (input.spin.held ? 12000.0f : 3000.0f) * seconds;
    const float forward[3]{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), std::cos(yaw_) * std::cos(pitch_)};
    const float right[3]{-std::cos(yaw_), 0, std::sin(yaw_)};
    const float rise = static_cast<float>(input.jump.held) - input.crouch.held;
    for (int i = 0; i < 3; ++i)
        position_[i] += (forward[i] * input.moveY + right[i] * input.moveX) * speed + (i == 1 ? rise * speed : 0);
    if (gravity_) {
        // The scene's movement, as the game runs it each frame: switch and sleep state follow here.
        gravity_->movement();
        gravityHere_ = describeGravity(*gravity_, stage_, position_);
        if (collision_) stepProbe(seconds);
    }
    if (figure_) animateFigure(seconds);
}

void SceneRenderer::drawInstance(const Instance& instance, const C3D_Mtx& view, bool translucent, const Material*& current) {
    const GpuModel& model = *instance.model;
    if (instance.sky || instance.light != currentLight_) {
        loadLightSet(lightSets_[instance.light], view);
        currentLight_ = instance.sky ? -1 : instance.light;
    }
    C3D_Mtx modelView;
    Mtx_Multiply(&modelView, &view, &instance.transform);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, modelView_, &modelView);
    const auto& materials = instance.materials ? *instance.materials : model.materials;
    for (const auto& shape : model.shapes) {
        const Material& m = materials[shape.material];
        if (m.translucent != translucent) continue;
        if (current != &m) {
            current = &m;
            for (unsigned i = 0; i < CombinerProgram::MaxStages; ++i) *C3D_GetTexEnv(i) = m.env[i];
            C3D_TexEnvBufColor(m.bufferColor);
            C3D_TexEnvBufUpdate(C3D_RGB, m.bufferUpdate[0]);
            C3D_TexEnvBufUpdate(C3D_Alpha, m.bufferUpdate[1]);
            for (unsigned unit = 0; unit < CombinerProgram::MaxUnits; ++unit) {
                if (m.textures[unit] >= 0) C3D_TexBind(unit, const_cast<C3D_Tex*>(&model.textures[m.textures[unit]]));
                for (int row = 0; row < 3; ++row) {
                    const auto& v = m.texgen[unit][row];
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, texgen_[unit] + row, v.x, v.y, v.z, v.w);
                }
            }
            C3D_FVUnifSet(GPU_VERTEX_SHADER, materialColor_, m.materialColor.x, m.materialColor.y, m.materialColor.z, m.materialColor.w);
            for (const auto& [location, v] : {std::pair{colorSelect_, m.colorSelect}, std::pair{alphaSelect_, m.alphaSelect},
                                              std::pair{colorLights_, m.colorLights}, std::pair{alphaLights_, m.alphaLights}})
                C3D_FVUnifSet(GPU_VERTEX_SHADER, location, v.x, v.y, v.z, v.w);
            C3D_CullFace(m.cull);
            C3D_AlphaTest(m.alphaTest, m.alphaFunc, m.alphaReference);
            C3D_AlphaBlend(m.equation, GPU_BLEND_ADD, m.source, m.destination, m.source, m.destination);
            if (instance.sky) C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            else C3D_DepthTest(m.depthTest, m.depthTest ? m.depthFunc : GPU_ALWAYS, m.depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
        }
        C3D_BufInfo* buffers = C3D_GetBufInfo();
        BufInfo_Init(buffers);
        BufInfo_Add(buffers, static_cast<PackedVertex*>(model.vertices) + shape.firstVertex, sizeof(PackedVertex), 4, 0x3210);
        C3D_DrawElements(GPU_TRIANGLES, shape.indexCount, C3D_UNSIGNED_SHORT,
                         static_cast<std::uint16_t*>(model.indices) + shape.firstIndex);
    }
}

void SceneRenderer::draw(C3D_RenderTarget* target) {
    if (!ready()) return;
    C3D_FrameDrawOn(target);
    C3D_BindProgram(&program_);
    C3D_AttrInfo* attrs = C3D_GetAttrInfo();
    AttrInfo_Init(attrs);
    AttrInfo_AddLoader(attrs, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrs, 1, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attrs, 2, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(attrs, 3, GPU_BYTE, 4);
    C3D_TexEnvBufUpdate(C3D_Both, 0);

    const float forward[3]{std::sin(yaw_) * std::cos(pitch_), std::sin(pitch_), std::cos(yaw_) * std::cos(pitch_)};
    C3D_Mtx view, projection;
    Mtx_LookAt(&view, FVec3_New(position_[0], position_[1], position_[2]),
               FVec3_New(position_[0] + forward[0], position_[1] + forward[1], position_[2] + forward[2]), FVec3_New(0, 1, 0), false);
    Mtx_PerspTilt(&projection, C3D_AngleFromDegrees(FieldOfView), C3D_AspectRatioTop, Near, Far, false);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, projection_, &projection);
    // Each placement's light set is loaded as it draws; the sky's view differs, so it loads its own.
    currentLight_ = -1;

    // The sky turns with the camera but never moves away from it, and is drawn first without depth.
    C3D_Mtx skyView = view;
    skyView.r[0].w = skyView.r[1].w = skyView.r[2].w = 0;
    const Material* current = nullptr;
    for (const auto& instance : instances_) {
        if (!instance.sky) continue;
        // Skies are far larger than the far plane; without depth, only their direction matters.
        Instance centered = instance;
        const float fit = instance.radius > Far / 2 ? Far / 2 / instance.radius : 1.0f;
        for (int row = 0; row < 3; ++row) {
            centered.transform.r[row].x *= fit;
            centered.transform.r[row].y *= fit;
            centered.transform.r[row].z *= fit;
            centered.transform.r[row].w = 0;
        }
        drawInstance(centered, skyView, false, current);
        drawInstance(centered, skyView, true, current);
    }

    const float tanY = std::tan(C3D_AngleFromDegrees(FieldOfView) / 2), tanX = tanY * C3D_AspectRatioTop;
    std::vector<std::pair<float, const Instance*>> visible;
    for (const auto& instance : instances_) {
        if (instance.sky) continue;
        const float* c = instance.center;
        const float x = view.r[0].x * c[0] + view.r[0].y * c[1] + view.r[0].z * c[2] + view.r[0].w;
        const float y = view.r[1].x * c[0] + view.r[1].y * c[1] + view.r[1].z * c[2] + view.r[1].w;
        const float depth = -(view.r[2].x * c[0] + view.r[2].y * c[1] + view.r[2].z * c[2] + view.r[2].w);
        const float r = instance.radius;
        if (depth + r < Near || depth - r > Far) continue;
        if (std::fabs(x) - r * std::sqrt(1 + tanX * tanX) > depth * tanX) continue;
        if (std::fabs(y) - r * std::sqrt(1 + tanY * tanY) > depth * tanY) continue;
        visible.push_back({depth, &instance});
    }
    for (const auto& [depth, instance] : visible) drawInstance(*instance, view, false, current);
    std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [depth, instance] : visible) drawInstance(*instance, view, true, current);

    // The probe, projected with the same view and field of view for the overlay to draw.
    const auto toScreen = [&](const float* p, std::array<float, 2>& out) {
        const float x = view.r[0].x * p[0] + view.r[0].y * p[1] + view.r[0].z * p[2] + view.r[0].w;
        const float y = view.r[1].x * p[0] + view.r[1].y * p[1] + view.r[1].z * p[2] + view.r[1].w;
        const float depth = -(view.r[2].x * p[0] + view.r[2].y * p[1] + view.r[2].z * p[2] + view.r[2].w);
        if (depth < Near) return false;
        out = {200 + x / (depth * tanX) * 200, 120 - y / (depth * tanY) * 120};
        return out[0] > -20 && out[0] < 420 && out[1] > -20 && out[1] < 260;
    };
    probeView_.trail.clear();
    probeView_.label = probeState_;
    probeView_.falling = probeFalling_;
    std::array<float, 2> at{};
    probeView_.visible = !probeState_.empty() && toScreen(probe_, at);
    probeView_.x = at[0], probeView_.y = at[1];
    for (const auto& point : probeTrail_)
        if (toScreen(point.data(), at)) probeView_.trail.push_back(at);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_CullFace(GPU_CULL_BACK_CCW);
}
} // namespace smg
