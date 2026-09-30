#include <smg/StageLight.hpp>
#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <smg/Scene.hpp>
#include <Game/Util/StringUtil.hpp>

namespace smg {
bool loadStageLights(const std::string& root, NativeScene& scene, std::string& error) {
    std::vector<std::uint8_t> storage;
    std::vector<ArchiveEntry> entries;
    if (!readArchive((root + "/ObjectData/LightData.arc").c_str(), storage, entries, 4u * 1024 * 1024, error)) {
        error = "LightData.arc: " + error;
        return false;
    }
    std::unique_ptr<Bcsv> data;
    std::vector<std::pair<std::string, std::unique_ptr<Bcsv>>> zones;
    for (const ArchiveEntry& entry : entries) {
        const std::string& path = entry.path;
        if (path.size() < 11 || !MR::isEqualStringCase(path.substr(path.size() - 5).c_str(), ".bcsv")) continue;
        auto table = std::make_unique<Bcsv>();
        if (!table->load({storage.data() + entry.offset, entry.size}, error)) {
            error = path + ": " + error;
            return false;
        }
        if (MR::isEqualStringCase(path.c_str(), "lightdata.bcsv")) data = std::move(table);
        else zones.emplace_back(path, std::move(table));
    }
    if (!data) {
        error = "LightData.arc has no LightData.bcsv";
        return false;
    }
    scene.initLights(std::move(data), std::move(zones));
    error.clear();
    return true;
}
} // namespace smg
