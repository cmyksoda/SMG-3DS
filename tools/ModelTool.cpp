#include <smg/Model.hpp>
#include <cstdio>

int main(int argc,char** argv) {
    if (argc!=3) { std::fprintf(stderr,"Usage: model_tool ARCHIVE MODEL_ENTRY\n"); return 2; }
    std::vector<std::uint8_t> bytes;
    std::vector<smg::ArchiveEntry> entries;
    std::string error;
    if (!smg::readArchive(argv[1],bytes,entries,64u*1024*1024,error)) {
        std::fprintf(stderr,"%s\n",error.c_str()); return 1;
    }
    for (const auto& entry : entries) if (entry.path==argv[2]) {
        smg::Model model;
        if (!smg::loadModel({bytes.data()+entry.offset,entry.size},model,32u*1024*1024,error)) {
            std::fprintf(stderr,"%s\n",error.c_str()); return 1;
        }
        std::printf("%zu triangles, %zu shapes, %zu materials, %zu textures\n",
            model.vertices.size()/3,model.shapes.size(),model.materials.size(),model.textures.size());
        std::printf("Bounds: (%g, %g, %g) to (%g, %g, %g)\n",model.minimum[0],model.minimum[1],model.minimum[2],
            model.maximum[0],model.maximum[1],model.maximum[2]);
        for (const auto& s : model.shapes) std::printf("Shape: %zu triangles, material %u\n",s.count/3,s.material);
        for (const auto& m : model.materials) std::printf("Material: texture %d, %u TEV stages\n",m.texture,m.tevStages);
        for (const auto& t : model.textures) std::printf("Texture: %ux%u\n",t.width,t.height);
        return 0;
    }
    std::fprintf(stderr,"Model entry not found\n"); return 1;
}
