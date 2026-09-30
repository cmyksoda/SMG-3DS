#include <smg/Stage.hpp>
#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <Game/Util/StringUtil.hpp>
#include <cmath>

namespace smg {
namespace {
constexpr std::size_t StageArchiveBudget = 8u * 1024 * 1024;

struct Archive {
    std::vector<std::uint8_t> storage;
    std::vector<ArchiveEntry> entries;
    bool open(const std::string& path, std::string& error) {
        return readArchive(path.c_str(), storage, entries, StageArchiveBudget, error);
    }
    // Missing tables are normal: most layers do not exist in most zones.
    bool table(const char* path, Bcsv& output, std::string& error) const {
        for (const auto& entry : entries) {
            if (!MR::isEqualStringCase(entry.path.c_str(), path)) continue;
            return output.load({storage.data() + entry.offset, entry.size}, error);
        }
        return false;
    }
};

float field(const JMapInfo& info, int row, const char* name, float fallback) {
    f32 value = fallback;
    return info.getValue(row, name, &value) && std::isfinite(value) ? value : fallback;
}

bool fail(std::string& error, const std::string& text) { error = text; return false; }

void place(const Archive& archive, const std::string& zone, std::int32_t mask, const Transform& zoneTransform,
           std::vector<StageObject>& objects) {
    for (const auto& layer : layerNames(mask)) {
        for (const auto& path : {"jmp/placement/" + layer + "/objinfo", "jmp/mapparts/" + layer + "/mappartsinfo"}) {
            Bcsv table;
            std::string ignored;
            if (!archive.table(path.c_str(), table, ignored)) continue;
            const auto& info = table.info();
            for (int row = 0; row < info.getNumEntries(); ++row) {
                const char* name = nullptr;
                if (!info.getValue(row, "name", &name) || !name) continue;
                const float position[3]{field(info, row, "pos_x", 0), field(info, row, "pos_y", 0), field(info, row, "pos_z", 0)};
                const float rotation[3]{field(info, row, "dir_x", 0), field(info, row, "dir_y", 0), field(info, row, "dir_z", 0)};
                const float scale[3]{field(info, row, "scale_x", 1), field(info, row, "scale_y", 1), field(info, row, "scale_z", 1)};
                objects.push_back({name, zone, multiply(zoneTransform, placementMatrix(position, rotation, scale))});
            }
        }
    }
}
} // namespace

std::vector<std::string> layerNames(std::int32_t mask) {
    std::vector<std::string> names{"common"};
    for (int bit = 0; bit < 16; ++bit)
        if (mask & (1 << bit)) names.push_back(std::string("layer") + static_cast<char>('a' + bit));
    return names;
}

Transform placementMatrix(const float position[3], const float rotation[3], const float scale[3]) {
    constexpr float toRadians = 3.14159265358979f / 180.0f;
    const float sx = std::sin(rotation[0] * toRadians), cx = std::cos(rotation[0] * toRadians);
    const float sy = std::sin(rotation[1] * toRadians), cy = std::cos(rotation[1] * toRadians);
    const float sz = std::sin(rotation[2] * toRadians), cz = std::cos(rotation[2] * toRadians);
    return {scale[0] * cz * cy, scale[1] * (cz * sy * sx - sz * cx), scale[2] * (cz * sy * cx + sz * sx), position[0],
            scale[0] * sz * cy, scale[1] * (sz * sy * sx + cz * cx), scale[2] * (sz * sy * cx - cz * sx), position[1],
            -scale[0] * sy, scale[1] * cy * sx, scale[2] * cy * cx, position[2]};
}

Transform multiply(const Transform& a, const Transform& b) {
    Transform r{};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            float sum = col == 3 ? a[row * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) sum += a[row * 4 + k] * b[k * 4 + col];
            r[row * 4 + col] = sum;
        }
    }
    return r;
}

bool loadStage(const std::string& root, const std::string& galaxy, int scenario, Stage& stage, std::string& error) {
    stage = {};
    stage.galaxy = galaxy;
    stage.scenario = scenario;
    Archive scenarios;
    if (!scenarios.open(root + "/StageData/" + galaxy + "/" + galaxy + "Scenario.arc", error)) return false;
    Bcsv scenarioTable, zoneTable;
    if (!scenarios.table("ScenarioData.bcsv", scenarioTable, error) || !scenarios.table("ZoneList.bcsv", zoneTable, error))
        return fail(error, "Scenario archive lacks its scenario or zone table");
    const auto& scenarioInfo = scenarioTable.info();
    int row = -1;
    for (int i = 0; i < scenarioInfo.getNumEntries(); ++i) {
        s32 number = 0;
        if (scenarioInfo.getValue(i, "ScenarioNo", &number) && number == scenario) row = i;
    }
    if (row < 0) return fail(error, "Scenario " + std::to_string(scenario) + " is not in " + galaxy);
    const auto maskOf = [&](const std::string& zone) {
        s32 mask = 0;
        scenarioInfo.getValue(row, zone.c_str(), &mask);
        return mask;
    };

    Archive map;
    if (!map.open(root + "/StageData/" + galaxy + ".arc", error)) return false;
    constexpr Transform identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    place(map, galaxy, maskOf(galaxy), identity, stage.objects);
    stage.zones.push_back({galaxy, identity, {}, {}, maskOf(galaxy)});

    // Entering from the galaxy map starts at the galaxy's MarioNo 0, searched through its active layers in order.
    std::string ignored;
    for (const auto& layer : layerNames(maskOf(galaxy))) {
        Bcsv start;
        if (!map.table(("jmp/start/" + layer + "/startinfo").c_str(), start, ignored)) continue;
        const JMapInfoIter entrance = start.info().findElement<s32>("MarioNo", 0, 0);
        if (entrance == start.info().end()) continue;
        for (int i = 0; i < 3; ++i) {
            const char position[]{'p', 'o', 's', '_', static_cast<char>('x' + i), 0};
            const char direction[]{'d', 'i', 'r', '_', static_cast<char>('x' + i), 0};
            stage.start[i] = field(start.info(), entrance.mIndex, position, 0);
            stage.startRotation[i] = field(start.info(), entrance.mIndex, direction, 0);
        }
        break;
    }

    // Zones are placed by the galaxy's own layers, then their contents by each zone's mask.
    for (const auto& layer : layerNames(maskOf(galaxy))) {
        Bcsv zones;
        if (!map.table(("jmp/placement/" + layer + "/stageobjinfo").c_str(), zones, ignored)) continue;
        const auto& info = zones.info();
        for (int i = 0; i < info.getNumEntries(); ++i) {
            const char* name = nullptr;
            if (!info.getValue(i, "name", &name) || !name) continue;
            const float position[3]{field(info, i, "pos_x", 0), field(info, i, "pos_y", 0), field(info, i, "pos_z", 0)};
            const float rotation[3]{field(info, i, "dir_x", 0), field(info, i, "dir_y", 0), field(info, i, "dir_z", 0)};
            const float unit[3]{1, 1, 1};
            Archive zone;
            if (!zone.open(root + "/StageData/" + name + ".arc", error)) return fail(error, std::string(name) + ": " + error);
            const Transform placement = placementMatrix(position, rotation, unit);
            place(zone, name, maskOf(name), placement, stage.objects);
            stage.zones.push_back({name, placement, {position[0], position[1], position[2]},
                                   {rotation[0], rotation[1], rotation[2]}, maskOf(name)});
        }
    }
    error.clear();
    return true;
}
} // namespace smg
