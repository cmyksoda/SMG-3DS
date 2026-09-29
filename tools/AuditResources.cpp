#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
bool extension(const std::string& name, const char* suffix) {
    std::string result = fs::path(name).extension().string();
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result == suffix;
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr,"Usage: resource_audit EXTRACTED_FILES_DIRECTORY\n"); return 2; }
    std::error_code ec;
    const fs::path root(argv[1]);
    if (!fs::is_directory(root, ec)) { std::fprintf(stderr,"Not a readable directory\n"); return 2; }
    std::vector<fs::path> archives;
    fs::recursive_directory_iterator it(root,ec), end;
    while (!ec && it != end) {
        if (it->is_regular_file(ec) && extension(it->path().string(),".arc")) archives.push_back(it->path());
        it.increment(ec);
    }
    if (ec) { std::fprintf(stderr,"Directory traversal: %s\n",ec.message().c_str()); return 2; }
    std::sort(archives.begin(), archives.end());
    std::size_t tables=0, records=0, resources=0, errors=0;
    for (const auto& path : archives) {
        std::vector<std::uint8_t> storage;
        std::vector<smg::ArchiveEntry> entries;
        std::string error;
        // Offline inspection budget, separate from the console's runtime memory budget.
        if (!smg::readArchive(path.c_str(),storage,entries,512u*1024*1024,error)) {
            std::fprintf(stderr,"ARCHIVE %s: %s\n",path.c_str(),error.c_str()); ++errors; continue;
        }
        resources += entries.size();
        for (const auto& entry : entries) if (extension(entry.path,".bcsv")) {
            ++tables;
            smg::Bcsv table;
            if (!table.load({storage.data()+entry.offset,entry.size},error)) {
                std::fprintf(stderr,"BCSV %s :: %s: %s\n",path.c_str(),entry.path.c_str(),error.c_str()); ++errors;
            } else records += table.info().getNumEntries();
        }
    }
    std::printf("%zu archives, %zu resources, %zu BCSV tables, %zu records, %zu errors\n",
        archives.size(),resources,tables,records,errors);
    if (archives.empty()) { std::fprintf(stderr,"No archives found\n"); return 2; }
    return errors == 0 ? 0 : 1;
}
