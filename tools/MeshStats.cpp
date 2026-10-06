#include <smg/Archive.hpp>
#include <smg/Combiner.hpp>
#include <smg/GxDraw.hpp>
#include <smg/Model.hpp>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// What the GPU path's compact mesh layout saves: every display list of the models in the archives given (one mesh each),
// under the material its shape draws with, packed as GxMeshLayout does against GxMeshVertex's 68 bytes a vertex.
namespace {
unsigned be16(const std::uint8_t* p) { return unsigned(p[0]) << 8 | p[1]; }
std::uint32_t be32(const std::uint8_t* p) { return std::uint32_t(be16(p)) << 16 | be16(p + 2); }

struct Totals {
    std::uint64_t meshes = 0, vertices = 0, indices = 0, packed = 0, refused = 0, refusedVertices = 0;
    std::map<unsigned, std::uint64_t> byStride, byInput;
    std::uint64_t lit = 0, color = 0, slot = 0, alias = 0, twoFloats = 0;
};

struct Shape {
    bool slot = false;
    std::uint16_t texMatrix = 0;  // TEXnMTXIDX attributes, a bit each
    struct List { std::uint32_t bytes = 0, vertices = 0, indices = 0; };
    std::vector<List> lists;
};

// SHP1's shapes: their matrix-index attributes and each display list's size, vertices and triangle indices.
bool readShapes(const std::uint8_t* shp, std::size_t size, std::vector<Shape>& shapes) {
    const unsigned count = be16(shp + 8);
    shapes.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        const std::uint8_t* init = shp + be32(shp + 12) + be16(shp + be32(shp + 16) + i * 2) * 40;
        Shape& shape = shapes[i];
        unsigned vertexSize = 0;
        for (const std::uint8_t* a = shp + be32(shp + 24) + be16(init + 4); a < shp + size && be32(a) != 255; a += 8) {
            const unsigned attribute = be32(a);
            if (attribute == 0) shape.slot = true;
            else if (attribute <= 8) shape.texMatrix = static_cast<std::uint16_t>(shape.texMatrix | 1u << (attribute - 1));
            vertexSize += be32(a + 4) == 3 ? 2 : 1;
        }
        for (unsigned group = 0; group < be16(init + 2); ++group) {
            const std::uint8_t* draw = shp + be32(shp + 40) + (be16(init + 8) + group) * 8;
            const std::uint8_t* list = shp + be32(shp + 32) + be32(draw + 4);
            Shape::List l;
            l.bytes = be32(draw);
            for (std::uint32_t at = 0; at < l.bytes;) {
                const unsigned op = list[at++];
                if (!op) continue;
                const unsigned n = be16(list + at);
                at += 2 + n * vertexSize;
                l.vertices += n;
                l.indices += op == 0x80 ? n / 4 * 6 : op == 0x90 ? n : n >= 2 ? (n - 2) * 3 : 0;
            }
            shape.lists.push_back(l);
        }
    }
    return true;
}

// The pipeline state a material and shape give a mesh, as J3D loads it into GX; false when the CPU path draws it.
bool layoutOf(const smg::ModelMaterial& m, const Shape& shape, smg::GxMeshLayout& layout) {
    const bool channel = m.channelCount != 0;
    const smg::ColorChannel &color = m.channels[0], &alpha = m.channels[1];
    const bool lit = channel && (color.lighting || alpha.lighting);
    const bool fromVertex = channel && (color.materialSource == 1 || alpha.materialSource == 1 ||
                                        (lit && (color.ambientSource == 1 || alpha.ambientSource == 1)));
    const smg::CombinerProgram program = smg::compileTev(m);
    smg::GxMeshLayout::Texgen texgens[smg::CombinerProgram::MaxUnits] = {};
    for (unsigned u = 0; u < program.unitCount; ++u) {
        const unsigned coord = program.units[u].texCoord;
        const smg::TexGen& t = m.texGens[coord];
        if (t.type > 1 || (t.source > 1 && (t.source < 4 || t.source > 11))) return false;
        const bool perVertexMatrix = shape.texMatrix >> coord & 1;
        if (perVertexMatrix && !(lit && shape.slot)) return false;
        texgens[u] = {static_cast<std::uint8_t>(t.source <= 1 ? t.source : t.source + 1), t.source <= 1, perVertexMatrix};
    }
    layout = smg::GxMeshLayout::make(fromVertex, lit, shape.slot, program.unitCount, texgens);
    return true;
}

void measure(const std::uint8_t* data, std::size_t size, const smg::Model& model, Totals& totals) {
    const std::uint8_t* shp = nullptr;
    std::size_t shpSize = 0;
    for (std::size_t at = 0x20, chunks = be32(data + 12); chunks-- && at + 8 <= size; at += be32(data + at + 4))
        if (!std::memcmp(data + at, "SHP1", 4)) shp = data + at, shpSize = be32(data + at + 4);
    std::vector<Shape> shapes;
    if (!shp || !readShapes(shp, shpSize, shapes) || shapes.size() != model.shapes.size()) return;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        smg::GxMeshLayout layout;
        const bool eligible = layoutOf(model.materials[model.shapes[i].material], shapes[i], layout);
        for (const Shape::List& l : shapes[i].lists) {
            if (l.bytes <= 32) continue;  // a block's worth is the CPU path's batch, not a mesh
            if (!eligible) {
                ++totals.refused, totals.refusedVertices += l.vertices;
                continue;
            }
            ++totals.meshes, totals.vertices += l.vertices, totals.indices += l.indices;
            totals.packed += std::uint64_t(l.vertices) * layout.stride;
            totals.byStride[layout.stride] += l.vertices;
            totals.lit += layout.lit * l.vertices, totals.color += (layout.color != smg::GxMeshLayout::Absent) * l.vertices;
            totals.slot += (layout.slot != smg::GxMeshLayout::Absent) * l.vertices;
            for (unsigned u = 0; u < 3; ++u) {
                totals.byInput[layout.unit[u].input] += l.vertices;
                totals.alias += layout.unit[u].input >= smg::GxMeshLayout::OfPosition ? l.vertices : 0;
                totals.twoFloats += layout.unit[u].input != smg::GxMeshLayout::Unused && layout.unit[u].floats == 2 ? l.vertices : 0;
            }
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: mesh_stats ARCHIVE...\n");
        return 2;
    }
    Totals totals;
    unsigned models = 0;
    for (int a = 1; a < argc; ++a) {
        std::vector<std::uint8_t> bytes;
        std::vector<smg::ArchiveEntry> entries;
        std::string error;
        if (!smg::readArchive(argv[a], bytes, entries, 64u * 1024 * 1024, error)) {
            std::fprintf(stderr, "%s: %s\n", argv[a], error.c_str());
            continue;
        }
        for (const smg::ArchiveEntry& entry : entries) {
            const std::string& path = entry.path;
            if (path.size() < 4 || (path.compare(path.size() - 4, 4, ".bdl") && path.compare(path.size() - 4, 4, ".bmd"))) continue;
            smg::Model model;
            if (!smg::loadModel({bytes.data() + entry.offset, entry.size}, model, 64u * 1024 * 1024, error)) {
                std::fprintf(stderr, "%s %s: %s\n", argv[a], path.c_str(), error.c_str());
                continue;
            }
            ++models;
            measure(bytes.data() + entry.offset, entry.size, model, totals);
        }
    }
    const double full = 68.0 * totals.vertices, packed = double(totals.packed);
    std::printf("%u models, %llu meshes (%llu left to the CPU path, %llu vertices), %llu vertices, %llu indices\n", models,
                static_cast<unsigned long long>(totals.meshes), static_cast<unsigned long long>(totals.refused),
                static_cast<unsigned long long>(totals.refusedVertices), static_cast<unsigned long long>(totals.vertices),
                static_cast<unsigned long long>(totals.indices));
    if (!totals.vertices) return 0;
    const double indexBytes = 2.0 * totals.indices;
    std::printf("vertices: %.2f MB now, %.2f MB packed (%.1f bytes a vertex against 68); with indices %.2f MB -> %.2f MB (%.0f%% saved)\n", full / 1048576,
                packed / 1048576, packed / totals.vertices, (full + indexBytes) / 1048576, (packed + indexBytes) / 1048576,
                100 * (full - packed) / (full + indexBytes));
    std::printf("by vertex: lit %.0f%%, color from the vertex %.0f%%, per-vertex matrix %.0f%%\n", 100.0 * totals.lit / totals.vertices,
                100.0 * totals.color / totals.vertices, 100.0 * totals.slot / totals.vertices);
    std::printf("texgen units per vertex: ");
    static const char* const names[] = {"unused", "stored", "of position", "of normal"};
    for (const auto& [input, vertices] : totals.byInput) std::printf("%s %.2f  ", names[input], double(vertices) / totals.vertices);
    std::printf("; two floats %.2f\n", double(totals.twoFloats) / totals.vertices);
    for (const auto& [stride, vertices] : totals.byStride)
        std::printf("  stride %2u: %5.1f%% of vertices\n", stride, 100.0 * vertices / totals.vertices);
    return 0;
}
