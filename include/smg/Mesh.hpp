#pragma once
#include <smg/Model.hpp>
#include <cstdint>
#include <vector>

namespace smg {
// The vertex layout the model shader reads; colors and normals are normalized bytes.
struct PackedVertex {
    float position[3];
    float uv[2][2];
    std::uint8_t color[4];
    std::int8_t normal[4];
};
static_assert(sizeof(PackedVertex) == 36, "shader attribute layout");

struct PackedShape {
    std::size_t firstVertex = 0, firstIndex = 0, indexCount = 0;
    unsigned material = 0;
};

struct PackedMesh {
    std::vector<PackedVertex> vertices;
    std::vector<std::uint16_t> indices; // relative to their shape's firstVertex
    std::vector<PackedShape> shapes;
    float center[3]{}, radius = 0;
};

PackedVertex packVertex(const ModelVertex& vertex);

// Shares identical vertices within each shape; shapes past 65536 vertices are split. With a kept skin, only
// vertices on the same draw matrix are shared, and `origin` gets the model vertex each packed one came from.
PackedMesh packMesh(const Model& model, std::vector<std::uint32_t>* origin = nullptr);
} // namespace smg
