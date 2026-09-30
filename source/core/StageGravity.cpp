#include <smg/StageGravity.hpp>
#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <smg/Scene.hpp>
#include <Game/Util/MtxUtil.hpp>
#include <Game/Util/StringUtil.hpp>
#include <algorithm>
#include <memory>

namespace smg {
namespace {
constexpr std::size_t ZoneArchiveBudget = 8u * 1024 * 1024;
}

bool placeStageGravity(const std::string& root, const Stage& stage, NativeScene& scene, std::string& error) {
    // Rows are matched to zones by their table, so every table lives until the last row is placed.
    std::vector<std::unique_ptr<Bcsv>> tables;
    for (const StageZone& zone : stage.zones) {
        std::vector<std::uint8_t> storage;
        std::vector<ArchiveEntry> entries;
        if (!readArchive((root + "/StageData/" + zone.name + ".arc").c_str(), storage, entries, ZoneArchiveBudget, error)) {
            error = zone.name + ": " + error;
            return false;
        }
        // StageDataHolder::calcPlacementMtx; the galaxy's own zone stays at the origin.
        TPos3f placement;
        MR::makeMtxTR(placement.toMtxPtr(), TVec3f(zone.position[0], zone.position[1], zone.position[2]),
                      TVec3f(zone.rotation[0], zone.rotation[1], zone.rotation[2]));
        const int id = scene.addZone(placement);
        // Paths are not layered; a wire gravity reads its rail from them.
        const auto find = [&](const std::string& path) {
            return std::find_if(entries.begin(), entries.end(),
                                [&](const ArchiveEntry& e) { return MR::isEqualStringCase(e.path.c_str(), path.c_str()); });
        };
        if (const auto paths = find("jmp/path/commonpathinfo"); paths != entries.end()) {
            auto table = std::make_unique<Bcsv>();
            if (!table->load({storage.data() + paths->offset, paths->size}, error)) {
                error = zone.name + " paths: " + error;
                return false;
            }
            std::vector<const JMapInfo*> points;
            for (int rail = 0; rail < table->info().getNumEntries(); ++rail) {
                const auto entry = find("jmp/path/commonpathpointinfo." + std::to_string(rail));
                if (entry == entries.end()) { points.push_back(nullptr); continue; }
                auto pointTable = std::make_unique<Bcsv>();
                if (!pointTable->load({storage.data() + entry->offset, entry->size}, error)) {
                    error = zone.name + " rail points: " + error;
                    return false;
                }
                scene.addZoneTable(id, pointTable->info());
                points.push_back(&pointTable->info());
                tables.push_back(std::move(pointTable));
            }
            scene.addZonePaths(id, table->info(), std::move(points));
            scene.addZoneTable(id, table->info());
            tables.push_back(std::move(table));
        }
        for (const std::string& layer : layerNames(zone.layerMask)) {
            const std::string path = "jmp/placement/" + layer + "/planetobjinfo";
            for (const ArchiveEntry& entry : entries) {
                if (!MR::isEqualStringCase(entry.path.c_str(), path.c_str())) continue;
                auto table = std::make_unique<Bcsv>();
                if (!table->load({storage.data() + entry.offset, entry.size}, error)) {
                    error = zone.name + " " + path + ": " + error;
                    return false;
                }
                scene.addZoneTable(id, table->info());
                for (int row = 0; row < table->info().getNumEntries(); ++row)
                    scene.placeGlobalGravity(JMapInfoIter(&table->info(), row));
                tables.push_back(std::move(table));
            }
        }
    }
    scene.forgetZoneTables();
    error.clear();
    return true;
}
} // namespace smg
