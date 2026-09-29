#pragma once
#include <smg/Archive.hpp>
#include <array>

namespace smg {
struct ModelVertex {
    float position[3]{};
    float normal[3]{0,1,0};
    float color[4]{1,1,1,1};
    float uv[2]{};
};
struct ModelTexture {
    unsigned width=0, height=0, wrapS=0, wrapT=0;
    // Row-major RGBA words (0xRRGGBBAA), top row first.
    std::vector<std::uint32_t> pixels;
};
struct ModelMaterial {
    int texture=-1;
    unsigned cullMode=2, tevStages=0;
    std::array<float,4> color{1,1,1,1};
    std::array<float,6> uvTransform{1,0,0,0,1,0};
};
struct ModelShape { std::size_t first=0, count=0; unsigned material=0; };
struct Model {
    std::vector<ModelVertex> vertices;
    std::vector<ModelShape> shapes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    std::array<float,3> minimum{}, maximum{};
};
// Initial static J3D path. Unsupported skeletal/vertex formats fail explicitly.
// Material data currently exposes the first texture, not a complete TEV translation.
bool loadModel(Bytes source, Model& output, std::size_t decodedBudget, std::string& error);
bool decodeCmpr(Bytes source, unsigned width, unsigned height, ModelTexture& output, std::string& error);
}
