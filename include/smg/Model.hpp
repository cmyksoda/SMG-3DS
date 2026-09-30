#pragma once
#include <smg/Archive.hpp>
#include <smg/Texture.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace smg {
struct ModelVertex {
    float position[3]{};
    float normal[3]{0, 1, 0};
    float color[4]{1, 1, 1, 1};
    float uv[2][2]{};
};

struct ModelTexture {
    std::string name;
    GxFormat format = GxFormat::RGBA8;
    Image image;
    unsigned wrapS = 0, wrapT = 0, minFilter = 1, magFilter = 1;
};

// GX state kept as the raw enumerations J3D stores; the renderer translates them.
struct TevStage {
    std::uint8_t colorIn[4]{15, 15, 15, 0}, colorOp = 0, colorBias = 0, colorScale = 0, colorClamp = 1, colorReg = 0;
    std::uint8_t alphaIn[4]{7, 7, 7, 0}, alphaOp = 0, alphaBias = 0, alphaScale = 0, alphaClamp = 1, alphaReg = 0;
    std::uint8_t texCoord = 0xff, texMap = 0xff, channel = 0xff;
    std::uint8_t konstColor = 0x0c, konstAlpha = 0x1c;
    std::uint8_t rasSwap = 0, texSwap = 0;
};
struct ColorChannel {
    bool lighting = false;
    std::uint8_t materialSource = 0, ambientSource = 0, lightMask = 0, diffuse = 0, attenuation = 2;
};
struct TexGen { std::uint8_t type = 1, source = 4, matrix = 60; };
struct TexMatrix {
    std::uint8_t projection = 0, mapping = 0;
    float center[3]{}, scale[2]{1, 1}, rotation = 0, translation[2]{};
    // Row-major 2x3 transform applied to (s, t, 1).
    std::array<float, 6> uv{1, 0, 0, 0, 1, 0};
};
struct ModelMaterial {
    std::string name;
    std::uint8_t mode = 1; // J3D draw pass: 1 opaque, 2 alpha test, 4 translucent
    unsigned cullMode = 2;
    std::uint8_t channelCount = 1;
    ColorChannel channels[4]; // color0, alpha0, color1, alpha1
    std::array<float, 4> materialColor[2]{{1, 1, 1, 1}, {1, 1, 1, 1}};
    std::array<float, 4> ambientColor[2]{{0, 0, 0, 0}, {0, 0, 0, 0}};
    std::uint8_t texGenCount = 0;
    TexGen texGens[8];
    TexMatrix texMatrices[10];
    bool hasTexMatrix[10]{};
    int textures[8]{-1, -1, -1, -1, -1, -1, -1, -1};
    std::uint8_t tevStageCount = 1;
    TevStage stages[16];
    std::array<float, 4> tevColors[4]{}, konstColors[4]{};
    std::uint8_t swapTables[4][4]{{0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}};
    struct { std::uint8_t compare0 = 7, reference0 = 0, op = 0, compare1 = 7, reference1 = 0; } alphaTest;
    struct { std::uint8_t type = 0, source = 1, destination = 0, logic = 0; } blend;
    struct { bool enable = true; std::uint8_t function = 3; bool update = true; } depth;
};

struct ModelShape {
    std::size_t first = 0, count = 0;
    unsigned material = 0;
    std::uint8_t matrixType = 0; // 0 rigid, 1 billboard, 2 Y billboard, 3 skinned
    int joint = -1;              // the joint whose mesh it is, as MR::hideJoint hides it
};
struct ModelJoint {
    std::string name;
    int parent = -1;
    float world[12]{}; // bind pose, row-major 3x4
    float scale[3]{1, 1, 1}, translation[3]{};
    std::int16_t rotation[3]{}; // the bind pose's own transform, in 1/32768ths of a half turn
    bool compensate = false;    // Maya's scale compensation: the parent's own scale does not reach it
};

// What skinning needs, kept when loadModel is asked for it: each vertex before its draw matrix, which draw
// matrix that is, and what each draw matrix is made of (one joint, or joints weighted over inverse binds).
struct ModelSkin {
    std::vector<ModelVertex> rest;       // per model vertex, before its draw matrix
    std::vector<std::uint16_t> matrix;   // per model vertex
    struct Draw {
        bool weighted = false;
        std::uint16_t index = 0; // DRW1's: the joint, or the EVP1 envelope when weighted
        std::vector<std::pair<std::uint16_t, float>> joints;
    };
    std::vector<Draw> draws;
    std::vector<std::array<float, 12>> inverseBind; // per joint; identity for joints no envelope uses
};

struct Model {
    std::vector<ModelVertex> vertices; // bind-pose triangles in model space
    std::vector<ModelShape> shapes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    std::vector<ModelJoint> joints;
    std::array<float, 3> minimum{}, maximum{};
    bool skinned = false; // uses weighted envelopes, so it needs animation to look right
    unsigned jointMode = 0; // INF1's: how J3D composes joints (0 basic, 1 Softimage, 2 Maya)
    ModelSkin skin;       // empty unless loadModel was asked to keep it
};

bool loadModel(Bytes source, Model& output, std::size_t decodedBudget, std::string& error, bool keepSkin = false);

// A joint's own transform, as JNT1 stores it and J3DAnmTransform poses it.
struct JointTransform {
    float scale[3]{1, 1, 1};
    std::int16_t rotation[3]{};
    float translation[3]{};
};
// Each joint's world matrix for a pose, composed down the hierarchy as J3D's joint calc does in the model's mode.
void poseSkeleton(const Model& model, const std::vector<JointTransform>& local, std::vector<std::array<float, 12>>& world);
// Each draw matrix for those joints: its joint's world, or its envelope's weighted skin. Needs the kept skin.
void skinMatrices(const Model& model, const std::vector<std::array<float, 12>>& world, std::vector<std::array<float, 12>>& draws);
// One model vertex from its rest through its draw matrix.
ModelVertex skinVertex(const Model& model, const std::vector<std::array<float, 12>>& draws, std::size_t vertex);
// Every vertex through its draw matrix for those joints.
void skinModel(const Model& model, const std::vector<std::array<float, 12>>& world, std::vector<ModelVertex>& vertices);
} // namespace smg
