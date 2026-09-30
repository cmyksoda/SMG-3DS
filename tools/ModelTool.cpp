#include <smg/Combiner.hpp>
#include <smg/Model.hpp>
#include <cstdio>
#include <array>
#include <cstring>

namespace {
void printMaterial(const smg::ModelMaterial& m) {
    std::printf("Material %s: mode %u, cull %u, %u channel(s), %u texgen(s), %u TEV stage(s)\n",
        m.name.c_str(), m.mode, m.cullMode, m.channelCount, m.texGenCount, m.tevStageCount);
    std::printf("  textures");
    for (int t : m.textures) if (t >= 0) std::printf(" %d", t);
    std::printf("; color0 lit %d src %u amb %u mask %02x; alpha0 lit %d src %u; alpha test %u/%u %u %u/%u; blend %u %u %u; depth %d %u %d\n",
        m.channels[0].lighting, m.channels[0].materialSource, m.channels[0].ambientSource, m.channels[0].lightMask,
        m.channels[1].lighting, m.channels[1].materialSource, m.alphaTest.compare0, m.alphaTest.reference0, m.alphaTest.op, m.alphaTest.compare1, m.alphaTest.reference1,
        m.blend.type, m.blend.source, m.blend.destination, m.depth.enable, m.depth.function, m.depth.update);
    const auto print = [](const char* label, const std::array<float, 4>* colors, unsigned count) {
        std::printf("  %s", label);
        for (unsigned i = 0; i < count; ++i)
            std::printf(" (%.2f %.2f %.2f %.2f)", colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
        std::printf("\n");
    };
    print("material", m.materialColor, 2);
    print("ambient", m.ambientColor, 2);
    print("tev", m.tevColors, 4);
    print("konst", m.konstColors, 4);
    for (unsigned i = 0; i < m.texGenCount; ++i)
        std::printf("  texgen %u: type %u src %u mtx %u\n", i, m.texGens[i].type, m.texGens[i].source, m.texGens[i].matrix);
    for (unsigned i = 0; i < m.tevStageCount; ++i) {
        const auto& s = m.stages[i];
        std::printf("  tev %u: coord %u map %u chan %u | C %u %u %u %u op %u bias %u scale %u clamp %u -> %u | "
                    "A %u %u %u %u op %u bias %u scale %u clamp %u -> %u | k %02x/%02x swap %u/%u\n",
            i, s.texCoord, s.texMap, s.channel, s.colorIn[0], s.colorIn[1], s.colorIn[2], s.colorIn[3],
            s.colorOp, s.colorBias, s.colorScale, s.colorClamp, s.colorReg, s.alphaIn[0], s.alphaIn[1], s.alphaIn[2],
            s.alphaIn[3], s.alphaOp, s.alphaBias, s.alphaScale, s.alphaClamp, s.alphaReg, s.konstColor, s.konstAlpha, s.rasSwap, s.texSwap);
    }
    const auto program = smg::compileTev(m);
    std::printf("  combiner: %u stage(s), %s\n", program.stageCount, program.exact ? "exact" : program.approximation);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4 || (argc == 4 && std::strcmp(argv[3], "--materials"))) {
        std::fprintf(stderr, "Usage: model_tool ARCHIVE MODEL_ENTRY [--materials]\n");
        return 2;
    }
    std::vector<std::uint8_t> bytes;
    std::vector<smg::ArchiveEntry> entries;
    std::string error;
    if (!smg::readArchive(argv[1], bytes, entries, 64u * 1024 * 1024, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    for (const auto& entry : entries) {
        if (entry.path != argv[2]) continue;
        smg::Model model;
        if (!smg::loadModel({bytes.data() + entry.offset, entry.size}, model, 64u * 1024 * 1024, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("%zu triangles, %zu shapes, %zu materials, %zu textures, %zu joints\n", model.vertices.size() / 3,
            model.shapes.size(), model.materials.size(), model.textures.size(), model.joints.size());
        std::printf("Bounds: (%g, %g, %g) to (%g, %g, %g)\n", model.minimum[0], model.minimum[1], model.minimum[2],
            model.maximum[0], model.maximum[1], model.maximum[2]);
        for (const auto& s : model.shapes)
            std::printf("Shape: %zu triangles, material %u, matrix type %u, joint %s\n", s.count / 3, s.material, s.matrixType,
                        s.joint >= 0 ? model.joints[s.joint].name.c_str() : "none");
        for (const auto& t : model.textures)
            std::printf("Texture %s: %ux%u format %u wrap %u/%u\n", t.name.c_str(), t.image.width, t.image.height,
                static_cast<unsigned>(t.format), t.wrapS, t.wrapT);
        if (argc == 4) for (const auto& m : model.materials) printMaterial(m);
        return 0;
    }
    std::fprintf(stderr, "Model entry not found\n");
    return 1;
}
