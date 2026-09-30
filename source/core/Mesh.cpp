#include <smg/Mesh.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace smg {
PackedVertex packVertex(const ModelVertex& v) {
    PackedVertex p{};
    std::memcpy(p.position, v.position, sizeof(p.position));
    std::memcpy(p.uv, v.uv, sizeof(p.uv));
    for (int i = 0; i < 4; ++i) p.color[i] = static_cast<std::uint8_t>(std::lround(std::clamp(v.color[i], 0.0f, 1.0f) * 255));
    for (int i = 0; i < 3; ++i) p.normal[i] = static_cast<std::int8_t>(std::lround(std::clamp(v.normal[i], -1.0f, 1.0f) * 127));
    return p;
}

namespace {
struct Key {
    PackedVertex v;
    std::uint32_t matrix; // vertices that move apart when animated must stay apart
    bool operator==(const Key& o) const { return matrix == o.matrix && !std::memcmp(&v, &o.v, sizeof(v)); }
};
struct Hash {
    std::size_t operator()(const Key& k) const {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&k.v);
        std::size_t h = (1469598103934665603ull & SIZE_MAX) ^ k.matrix;
        for (std::size_t i = 0; i < sizeof(k.v); ++i) h = (h ^ bytes[i]) * (1099511628211ull & SIZE_MAX);
        return h;
    }
};
} // namespace

PackedMesh packMesh(const Model& model, std::vector<std::uint32_t>* origin) {
    PackedMesh mesh;
    if (origin) origin->clear();
    const bool skin = model.skin.matrix.size() == model.vertices.size();
    for (std::size_t s = 0; s < model.shapes.size(); ++s) {
        const auto& shape = model.shapes[s];
        std::unordered_map<Key, std::uint16_t, Hash> seen;
        PackedShape current{mesh.vertices.size(), mesh.indices.size(), 0, shape.material};
        for (std::size_t i = shape.first; i + 3 <= shape.first + shape.count; i += 3) {
            // A whole triangle must fit, so start a new shape before the 16-bit indices run out.
            if (mesh.vertices.size() - current.firstVertex > 65536 - 3) {
                current.indexCount = mesh.indices.size() - current.firstIndex;
                mesh.shapes.push_back(current);
                current = {mesh.vertices.size(), mesh.indices.size(), 0, shape.material};
                seen.clear();
            }
            for (std::size_t k = 0; k < 3; ++k) {
                const Key key{packVertex(model.vertices[i + k]), skin ? model.skin.matrix[i + k] : 0u};
                auto [it, inserted] = seen.emplace(key, static_cast<std::uint16_t>(mesh.vertices.size() - current.firstVertex));
                if (inserted) {
                    mesh.vertices.push_back(key.v);
                    if (origin) origin->push_back(static_cast<std::uint32_t>(i + k));
                }
                mesh.indices.push_back(it->second);
            }
        }
        current.indexCount = mesh.indices.size() - current.firstIndex;
        if (current.indexCount) mesh.shapes.push_back(current);
    }
    if (!model.vertices.empty()) {
        for (int i = 0; i < 3; ++i) mesh.center[i] = (model.minimum[i] + model.maximum[i]) / 2;
        float radius = 0;
        for (const auto& v : model.vertices) {
            const float dx = v.position[0] - mesh.center[0], dy = v.position[1] - mesh.center[1], dz = v.position[2] - mesh.center[2];
            radius = std::max(radius, dx * dx + dy * dy + dz * dz);
        }
        mesh.radius = std::sqrt(radius);
    }
    return mesh;
}
} // namespace smg
