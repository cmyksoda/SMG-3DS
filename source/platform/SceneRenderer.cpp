#include <smg/SceneRenderer.hpp>
#include <ModelShader.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace smg {
namespace {
unsigned morton(unsigned x, unsigned y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

GPU_TEXTURE_WRAP_PARAM wrap(unsigned value) {
    return value == 1 ? GPU_REPEAT : value == 2 ? GPU_MIRRORED_REPEAT : GPU_CLAMP_TO_EDGE;
}

bool upload(C3D_Tex& texture, const ModelTexture& source) {
    const bool powerOfTwo = !(source.width & (source.width - 1)) && !(source.height & (source.height - 1));
    if (source.width < 8 || source.height < 8 || !powerOfTwo) return false;
    if (!C3D_TexInit(&texture, source.width, source.height, GPU_RGBA8)) return false;
    auto* dst = static_cast<std::uint32_t*>(texture.data);
    for (unsigned y = 0; y < source.height; ++y) {
        for (unsigned x = 0; x < source.width; ++x) {
            const unsigned address = ((y / 8) * (source.width / 8) + x / 8) * 64 + morton(x, y);
            dst[address] = source.pixels[y * source.width + x];
        }
    }
    C3D_TexFlush(&texture);
    C3D_TexSetFilter(&texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&texture, wrap(source.wrapS), wrap(source.wrapT));
    return true;
}
} // namespace

bool SceneRenderer::load(const char* archivePath, const char* modelEntry) {
    shutdown();
    std::vector<std::uint8_t> storage;
    std::vector<ArchiveEntry> entries;
    Model model;
    if (!readArchive(archivePath, storage, entries, 32u * 1024 * 1024, status_)) return false;
    auto found = std::find_if(entries.begin(), entries.end(), [&](const ArchiveEntry& e) { return e.path == modelEntry; });
    if (found == entries.end()) { status_ = "Original model entry is missing"; return false; }
    if (!loadModel({storage.data() + found->offset, found->size}, model, 16u * 1024 * 1024, status_)) return false;
    std::vector<std::uint8_t>().swap(storage);

    shader_ = DVLB_ParseFile(reinterpret_cast<u32*>(modelShader), sizeof(modelShader));
    if (!shader_) { status_ = "Cannot parse native model shader"; shutdown(); return false; }
    shaderProgramInit(&program_);
    programReady_ = true;
    if (R_FAILED(shaderProgramSetVsh(&program_, &shader_->DVLE[0]))) {
        status_ = "Cannot create model shader"; shutdown(); return false;
    }
    projectionLocation_ = shaderInstanceGetUniformLocation(program_.vertexShader, "projection");
    viewLocation_ = shaderInstanceGetUniformLocation(program_.vertexShader, "modelView");
    colorLocation_ = shaderInstanceGetUniformLocation(program_.vertexShader, "materialColor");
    if (projectionLocation_ < 0 || viewLocation_ < 0 || colorLocation_ < 0) {
        status_ = "Missing model shader uniforms"; shutdown(); return false;
    }

    // The shader has no texture matrix, so bake each material's UV transform into its vertices.
    for (const auto& shape : model.shapes) {
        const auto& m = model.materials[shape.material];
        for (std::size_t i = shape.first; i < shape.first + shape.count; ++i) {
            auto& vertex = model.vertices[i];
            const float u = vertex.uv[0], v = vertex.uv[1];
            vertex.uv[0] = m.uvTransform[0] * u + m.uvTransform[1] * v + m.uvTransform[2];
            vertex.uv[1] = m.uvTransform[3] * u + m.uvTransform[4] * v + m.uvTransform[5];
        }
    }
    const std::size_t vertexBytes = model.vertices.size() * sizeof(ModelVertex);
    vertices_ = linearAlloc(vertexBytes);
    if (!vertices_) { status_ = "Cannot allocate model vertex buffer"; shutdown(); return false; }
    std::memcpy(vertices_, model.vertices.data(), vertexBytes);
    GSPGPU_FlushDataCache(vertices_, vertexBytes);

    textures_.resize(model.textures.size());
    for (std::size_t i = 0; i < model.textures.size(); ++i) {
        if (!upload(textures_[i], model.textures[i])) {
            status_ = "Cannot upload original texture"; shutdown(); return false;
        }
    }
    ModelTexture white;
    white.width = white.height = 8;
    white.pixels.assign(64, 0xffffffff);
    whiteReady_ = upload(white_, white);
    if (!whiteReady_) { status_ = "Cannot allocate material fallback texture"; shutdown(); return false; }

    float extent = 0;
    for (unsigned i = 0; i < 3; ++i) {
        center_[i] = (model.minimum[i] + model.maximum[i]) * 0.5f;
        extent = std::max(extent, model.maximum[i] - model.minimum[i]);
    }
    scale_ = extent > 0 ? 2.0f / extent : 1.0f;
    shapes_ = std::move(model.shapes);
    materials_ = std::move(model.materials);
    Mtx_PerspTilt(&projection_, C3D_AngleFromDegrees(50), C3D_AspectRatioTop, 0.1f, 20.0f, false);
    status_ = std::to_string(model.vertices.size() / 3) + " original triangles / base material preview";
    return true;
}

void SceneRenderer::shutdown() {
    if (vertices_) linearFree(vertices_);
    vertices_ = nullptr;
    for (auto& texture : textures_) if (texture.data) C3D_TexDelete(&texture);
    textures_.clear();
    if (whiteReady_) C3D_TexDelete(&white_);
    whiteReady_ = false;
    if (programReady_) shaderProgramFree(&program_);
    programReady_ = false;
    if (shader_) DVLB_Free(shader_);
    shader_ = nullptr;
    shapes_.clear();
    materials_.clear();
}

void SceneRenderer::update(const InputFrame& input, float seconds) {
    if (input.camera.pressed) { yaw_ = 0; pitch_ = 0.35f; }
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 0.25f) return;
    yaw_ += (static_cast<int>(input.dpadRight.held) - input.dpadLeft.held + input.cameraX * 2) * seconds;
    pitch_ += (static_cast<int>(input.dpadDown.held) - input.dpadUp.held - input.cameraY * 2) * seconds;
    pitch_ = std::clamp(pitch_, -1.5f, 1.5f);
}

void SceneRenderer::draw(C3D_RenderTarget* target) {
    if (!ready()) return;
    C3D_FrameDrawOn(target);
    C3D_BindProgram(&program_);
    C3D_AttrInfo* attrs = C3D_GetAttrInfo();
    AttrInfo_Init(attrs);
    AttrInfo_AddLoader(attrs, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrs, 1, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attrs, 2, GPU_FLOAT, 4);
    AttrInfo_AddLoader(attrs, 3, GPU_FLOAT, 2);
    C3D_BufInfo* buffers = C3D_GetBufInfo();
    BufInfo_Init(buffers);
    BufInfo_Add(buffers, vertices_, sizeof(ModelVertex), 4, 0x3210);

    C3D_Mtx view;
    Mtx_Identity(&view);
    Mtx_Translate(&view, 0, 0, -3.6f, true);
    Mtx_RotateX(&view, pitch_, true);
    Mtx_RotateY(&view, yaw_, true);
    Mtx_Scale(&view, scale_, scale_, scale_);
    Mtx_Translate(&view, -center_[0], -center_[1], -center_[2], true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, projectionLocation_, &projection_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, viewLocation_, &view);

    for (unsigned i = 0; i < 6; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    auto* env = C3D_GetTexEnv(0);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    for (const auto& shape : shapes_) {
        const auto& material = materials_[shape.material];
        C3D_CullFace(material.cullMode == 0 ? GPU_CULL_NONE : material.cullMode == 1 ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW);
        C3D_TexBind(0, material.texture >= 0 ? &textures_[material.texture] : &white_);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, colorLocation_, material.color[0], material.color[1],
                      material.color[2], material.color[3]);
        C3D_DrawArrays(GPU_TRIANGLES, shape.first, shape.count);
    }
}
} // namespace smg
