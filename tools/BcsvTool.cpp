#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr,"Usage: bcsv_tool ARCHIVE ENTRY FIELD [FIELD...]\n"); return 2; }
    std::vector<std::uint8_t> storage;
    std::vector<smg::ArchiveEntry> entries;
    std::string error;
    if (!smg::readArchive(argv[1],storage,entries,512u*1024*1024,error)) {
        std::fprintf(stderr,"%s\n",error.c_str()); return 1;
    }
    for (const auto& entry : entries) if (entry.path == argv[2]) {
        smg::Bcsv table;
        if (!table.load({storage.data()+entry.offset,entry.size},error)) {
            std::fprintf(stderr,"%s\n",error.c_str()); return 1;
        }
        const auto& info = table.info();
        for (int i=3;i<argc;++i) std::printf("%s%s",i==3?"":"\t",argv[i]);
        std::puts("");
        for (int row=0;row<info.getNumEntries();++row) {
            for (int i=3;i<argc;++i) {
                if (i>3) std::putchar('\t');
                const auto type = info.getValueType(argv[i]);
                if (type==JMAP_VALUE_TYPE_STRING || type==JMAP_VALUE_TYPE_STRING_PTR) {
                    const char* value=nullptr;
                    if (info.getValue(row,argv[i],&value)) std::printf("%s",value);
                } else if (type==JMAP_VALUE_TYPE_FLOAT) {
                    f32 value=0;
                    if (info.getValue(row,argv[i],&value)) std::printf("%g",value);
                } else {
                    s32 value=0; u32 unsignedValue=0;
                    if (info.getValue(row,argv[i],&value)) std::printf("%d",value);
                    else if (info.getValue(row,argv[i],&unsignedValue)) std::printf("%u",unsignedValue);
                }
            }
            std::puts("");
        }
        return 0;
    }
    std::fprintf(stderr,"Entry not found\n"); return 1;
}
