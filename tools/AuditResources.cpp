#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <smg/Combiner.hpp>
#include <smg/Kcl.hpp>
#include <smg/Model.hpp>
#include <smg/Scene.hpp>
#include <smg/Stage.hpp>
#include <smg/StageGravity.hpp>
#include <Game/Gravity/GravityInfo.hpp>
#include <Game/Gravity/PlanetGravity.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/Camera/CameraPolygonCodeUtil.hpp>
#include <Game/Map/KCollision.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <set>
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

bool used(const JMapInfo& info, int row, const char* field) {
    s32 id = -1;
    return info.getValue(row, field, &id) && id != -1;
}

// Every gravity row in every stage archive, then every scenario through placeStageGravity.
int auditGravity(const fs::path& root) {
    std::size_t errors = 0;
    struct Rows { std::size_t count = 0, switchA = 0, switchB = 0, sleep = 0, follower = 0; std::set<std::string> tables; };
    std::map<std::string, Rows> rows;
    std::size_t archives = 0, zonesWithGravity = 0;
    std::vector<fs::path> stageArchives;
    for (const auto& entry : fs::directory_iterator(root / "StageData"))
        if (entry.is_regular_file() && extension(entry.path().string(), ".arc")) stageArchives.push_back(entry.path());
    std::sort(stageArchives.begin(), stageArchives.end());
    for (const auto& path : stageArchives) {
        std::vector<std::uint8_t> storage;
        std::vector<smg::ArchiveEntry> entries;
        std::string error;
        if (!smg::readArchive(path.c_str(), storage, entries, 64u << 20, error)) {
            std::fprintf(stderr, "ARCHIVE %s: %s\n", path.c_str(), error.c_str());
            ++errors;
            continue;
        }
        ++archives;
        bool any = false;
        for (const auto& entry : entries) {
            if (entry.path.rfind("jmp/", 0) != 0) continue;
            smg::Bcsv table;
            if (!table.load({storage.data() + entry.offset, entry.size}, error)) continue;
            const auto& info = table.info();
            // "jmp/placement/layera/planetobjinfo" is filed under placement/planetobjinfo.
            const auto first = entry.path.find('/', 4), last = entry.path.rfind('/');
            const std::string kind = entry.path.substr(4, first - 4) + "/" + entry.path.substr(last + 1);
            for (int row = 0; row < info.getNumEntries(); ++row) {
                const char* name = nullptr;
                if (!info.getValue(row, "name", &name) || !name || !std::strstr(name, "Gravity")) continue;
                auto& r = rows[name];
                ++r.count;
                r.tables.insert(kind);
                r.switchA += used(info, row, "SW_A");
                r.switchB += used(info, row, "SW_B");
                r.sleep += used(info, row, "SW_SLEEP");
                r.follower += used(info, row, "FollowId");
                any = true;
            }
        }
        zonesWithGravity += any;
    }
    std::size_t global = 0, actors = 0;
    for (const auto& [name, r] : rows) (name.rfind("Global", 0) == 0 ? global : actors) += r.count;
    std::printf("Global gravity rows on the disc: %zu, and %zu rows of other objects named for gravity, in %zu of %zu "
                "stage archives\n", global, actors, zonesWithGravity, archives);
    std::printf("%6s %5s %5s %5s %5s  %s\n", "rows", "SW_A", "SW_B", "SLEEP", "Follow", "name (tables)");
    for (const auto& [name, r] : rows) {
        std::string tables;
        for (const auto& t : r.tables) tables += (tables.empty() ? "" : ", ") + t;
        std::printf("%6zu %5zu %5zu %5zu %5zu  %s (%s)\n", r.count, r.switchA, r.switchB, r.sleep, r.follower, name.c_str(), tables.c_str());
    }

    // Every scenario of every galaxy, as the preview loads it.
    std::vector<std::string> galaxies;
    for (const auto& entry : fs::directory_iterator(root / "StageData")) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && fs::exists(entry.path() / (name + "Scenario.arc"))) galaxies.push_back(name);
    }
    std::sort(galaxies.begin(), galaxies.end());
    std::size_t scenarios = 0, failed = 0, placedTotal = 0, startState = 0, sampled = 0;
    std::map<std::string, std::size_t> placed, skipped;
    std::vector<std::string> noStartGravity, withSkipped, noEntrance;
    for (const auto& galaxy : galaxies) {
        std::vector<std::uint8_t> storage;
        std::vector<smg::ArchiveEntry> entries;
        std::string error;
        const fs::path scenarioPath = root / "StageData" / galaxy / (galaxy + "Scenario.arc");
        std::vector<s32> numbers;
        std::set<s32> hidden;
        if (smg::readArchive(scenarioPath.c_str(), storage, entries, 8u << 20, error)) {
            for (const auto& entry : entries) {
                if (entry.path != "scenariodata.bcsv") continue;
                smg::Bcsv table;
                if (!table.load({storage.data() + entry.offset, entry.size}, error)) break;
                for (int row = 0; row < table.info().getNumEntries(); ++row) {
                    s32 number = 0, isHidden = 0;
                    u32 isHiddenUnsigned = 0;
                    if (!table.info().getValue(row, "ScenarioNo", &number)) continue;
                    numbers.push_back(number);
                    if ((table.info().getValue(row, "IsHidden", &isHidden) && isHidden) ||
                        (table.info().getValue(row, "IsHidden", &isHiddenUnsigned) && isHiddenUnsigned))
                        hidden.insert(number);
                }
            }
        }
        if (numbers.empty()) {
            std::fprintf(stderr, "GALAXY %s: no scenarios read (%s)\n", galaxy.c_str(), error.c_str());
            ++errors;
            continue;
        }
        for (const s32 number : numbers) {
            ++scenarios;
            const std::string label = galaxy + " " + std::to_string(number);
            smg::Stage stage;
            if (!smg::loadStage(root.string(), galaxy, number, stage, error)) {
                std::fprintf(stderr, "STAGE %s: %s\n", label.c_str(), error.c_str());
                ++failed;
                continue;
            }
            smg::NativeScene scene;
            if (!smg::placeStageGravity(root.string(), stage, scene, error)) {
                std::fprintf(stderr, "GRAVITY %s: %s\n", label.c_str(), error.c_str());
                ++failed;
                continue;
            }
            for (const auto& g : scene.placedGravities()) ++placed[g.name];
            for (const auto& name : scene.skippedGravities()) ++skipped[name];
            if (!scene.skippedGravities().empty()) withSkipped.push_back(label);
            placedTotal += scene.placedGravities().size();
            startState += scene.startStateGravities().size();

            // Every gravity asked for its pull around the start, near and far; none may be NaN or infinite.
            const TVec3f start(stage.start[0], stage.start[1], stage.start[2]);
            for (const auto& g : scene.placedGravities()) {
                for (int i = 0; i < 48; ++i) {
                    const float radius = i < 16 ? 300.0f : i < 32 ? 6000.0f : 60000.0f;
                    const float a = i * 2.39996f, z = 1 - 2 * ((i % 16) + 0.5f) / 16;
                    const float ring = std::sqrt(1 - z * z);
                    const TVec3f at(start.x + radius * ring * std::cos(a), start.y + radius * z, start.z + radius * ring * std::sin(a));
                    TVec3f pull;
                    ++sampled;
                    if (!g.gravity->calcGravity(&pull, at)) continue;
                    if (std::isfinite(pull.x) && std::isfinite(pull.y) && std::isfinite(pull.z)) continue;
                    std::fprintf(stderr, "PULL %s: %s in %s is not finite at (%g, %g, %g)\n", label.c_str(), g.name.c_str(),
                                 stage.zones[g.zone].name.c_str(), at.x, at.y, at.z);
                    ++errors;
                    break;
                }
            }
            TVec3f pull;
            GravityInfo info;
            auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
            if (!stage.entrance && !hidden.count(number)) {
                // Only a hidden star, reached from inside another scenario, may lack its own entrance.
                std::fprintf(stderr, "START %s: no MarioNo 0 entrance, and the scenario is not hidden\n", label.c_str());
                ++errors;
            }
            if (!stage.entrance) noEntrance.push_back(label);
            else if (!manager->calcTotalGravityVector(&pull, &info, start, GRAVITY_TYPE_NORMAL, 0)) noStartGravity.push_back(label);
        }
    }
    std::printf("\n%zu galaxies, %zu scenarios, %zu failed to place\n", galaxies.size(), scenarios, failed);
    std::printf("%zu gravities placed across all scenarios, %zu pulls sampled\n", placedTotal, sampled);
    for (const auto& [name, count] : placed) std::printf("%6zu  %s\n", count, name.c_str());
    for (const auto& [name, count] : skipped) std::printf("%6zu  skipped: %s\n", count, name.c_str());
    std::printf("%zu placements keep only their start state (switches, SW_SLEEP or FollowId)\n", startState);
    std::printf("%zu scenarios skip a gravity:", withSkipped.size());
    for (const auto& label : withSkipped) std::printf(" [%s]", label.c_str());
    std::printf("\n%zu hidden-star scenarios have no MarioNo 0 entrance in their layers:", noEntrance.size());
    for (const auto& label : noEntrance) std::printf(" [%s]", label.c_str());
    std::printf("\n%zu scenarios have no gravity at Mario's start:", noStartGravity.size());
    for (const auto& label : noStartGravity) std::printf(" [%s]", label.c_str());
    std::printf("\n");
    return errors + failed == 0 ? 0 : 1;
}

// Nearest front-face hit along the segment o + d t, t in [0, 1], over every enabled triangle.
float bruteForce(const KCollisionServer& server, const TVec3f& o, const TVec3f& d, int& which) {
    float best = 2;
    which = -1;
    for (int i = 0; i < server.getTriangleNum(); ++i) {
        const KC_PrismData* prism = server.getPrismData(i);
        if (prism->mHeight <= 0) continue;
        const TVec3f a = server.getPos(prism, 0), b = server.getPos(prism, 1), c = server.getPos(prism, 2);
        if (!std::isfinite(b.x + b.y + b.z + c.x + c.y + c.z)) continue;
        const TVec3f& n = *server.getFaceNormal(prism);
        if (n.dot(d) >= 0 || n.dot(o - a) <= 0) continue;
        const TVec3f e1 = b - a, e2 = c - a;
        TVec3f p;
        p.cross(d, e2);
        const float det = e1.dot(p);
        if (std::fabs(det) < 1e-12f) continue;
        const TVec3f q0 = o - a;
        const float u = q0.dot(p) / det;
        TVec3f q;
        q.cross(q0, e1);
        const float v = d.dot(q) / det, t = e2.dot(q) / det;
        if (u < 0 || v < 0 || u + v > 1 || t < 0 || t > 1 || t >= best) continue;
        best = t, which = i;
    }
    return best;
}

// How far inside its triangle a point in the plane is, by the prism's own edge normals.
float inset(const KCollisionServer& server, const KC_PrismData* prism, const TVec3f& point) {
    const TVec3f rel = point - server.getPos(prism, 0);
    return std::min({-rel.dot(*server.getEdgeNormal1(prism)), -rel.dot(*server.getEdgeNormal2(prism)),
                     prism->mHeight - rel.dot(*server.getEdgeNormal3(prism))});
}

// Whether the octree lists a triangle in the block holding a point, as the game's walk would find it.
bool listed(const KCollisionServer& server, const TVec3f& point, int triangle) {
    KCollisionServer::V3u cell;
    server.objectSpaceToLocalSpace(&cell, point);
    if (!server.isInsideMinMaxInLocalSpace(cell)) return false;
    s32 shift;
    const u16* list = reinterpret_cast<const u16*>(server.searchBlock(&shift, cell.x, cell.y, cell.z));
    while (*++list) if (*list == triangle + 1) return true;
    return false;
}

// Every KCL on the disc through the original KCollisionServer, rays checked against brute force.
int auditCollision(const fs::path& root) {
    std::vector<fs::path> archives;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && extension(entry.path().string(), ".arc")) archives.push_back(entry.path());
    std::sort(archives.begin(), archives.end());
    std::size_t files = 0, triangles = 0, failed = 0, noAttributes = 0, badAttributes = 0, rays = 0, hits = 0, edges = 0,
                wrong = 0, disabled = 0, cameraCodes = 0, unlisted = 0, firstBlock = 0;
    for (const auto& path : archives) {
        std::vector<std::uint8_t> storage;
        std::vector<smg::ArchiveEntry> entries;
        std::string error;
        if (!smg::readArchive(path.c_str(), storage, entries, 64u << 20, error)) continue;
        for (const auto& entry : entries) {
            if (!extension(entry.path, ".kcl")) continue;
            ++files;
            const std::string label = path.filename().string() + " :: " + entry.path;
            smg::Kcl kcl;
            if (!kcl.load({storage.data() + entry.offset, entry.size}, error)) {
                std::fprintf(stderr, "KCL %s: %s\n", label.c_str(), error.c_str());
                ++failed;
                continue;
            }
            triangles += kcl.triangles();
            const std::string pa = entry.path.substr(0, entry.path.size() - 3) + "pa";
            smg::Bcsv attributes;
            const auto table = std::find_if(entries.begin(), entries.end(), [&](const smg::ArchiveEntry& e) { return e.path == pa; });
            if (table == entries.end() || !attributes.load({storage.data() + table->offset, table->size}, error)) {
                ++noAttributes;
                std::fprintf(stderr, "PA %s: no attribute table\n", label.c_str());
            }
            // As CollisionParts::init sets up each part's server.
            smg::NativeScene scene;
            KCollisionServer server;
            server.init(kcl.data(), attributes.info().mData);
            MR::initCameraCodeCollection(label.c_str(), 0);
            if (!server.calcFarthestVertexDistance() && table != entries.end()) {
                ++badAttributes;
                std::fprintf(stderr, "PA %s: a triangle names a missing attribute row\n", label.c_str());
            }
            MR::termCameraCodeCollection();
            cameraCodes += scene.cameraCodes().back().codes.size();
            for (int i = 0; i < server.getTriangleNum(); ++i) disabled += server.getPrismData(i)->mHeight <= 0;

            TVec3f lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            for (int i = 0; i < server.getTriangleNum(); ++i) {
                for (int k = 0; k < 3; ++k) {
                    const TVec3f p = server.getPos(server.getPrismData(i), k);
                    lo.set(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                    hi.set(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
                }
            }
            std::mt19937 rng(static_cast<unsigned>(files));
            std::uniform_real_distribution<float> unit(0, 1);
            const auto inside = [&](float grow) {
                const TVec3f size = hi - lo;
                return TVec3f(lo.x - grow * size.x + unit(rng) * size.x * (1 + 2 * grow),
                              lo.y - grow * size.y + unit(rng) * size.y * (1 + 2 * grow),
                              lo.z - grow * size.z + unit(rng) * size.z * (1 + 2 * grow));
            };
            for (int ray = 0; ray < 48; ++ray, ++rays) {
                const TVec3f from = inside(0.3f), to = inside(0);
                const TVec3f d = to - from;
                f32 fraction = 2;
                u8 flag = 0;
                const KC_PrismData* prism = server.checkArrow(from, d, &fraction, &flag, nullptr, nullptr, 0);
                int expected = -1;
                const float brute = bruteForce(server, from, d, expected);
                const float length = d.length();
                if (!prism && expected < 0) continue;
                if (prism && expected >= 0 && std::fabs(fraction - brute) * length <= 0.05f) { ++hits; continue; }
                // KCHitArrow keeps hits up to 0.01 outside an edge; brute force does not.
                const KC_PrismData* near = prism ? prism : server.getPrismData(expected);
                const float t = prism ? fraction : brute;
                if (inset(server, near, from + d * t) > -0.05f && inset(server, near, from + d * t) < 0.05f) { ++edges; continue; }
                if (prism && expected >= 0 && fraction < brute && inset(server, prism, from + d * fraction) > -0.02f) { ++edges; continue; }
                // Nintendo's octrees leave some triangles out of blocks they barely touch; the game misses them too.
                if (expected >= 0 && (!prism || brute < fraction) && !listed(server, from + d * brute, expected)) { ++unlisted; continue; }
                // checkArrow stops at the first block with any hit, even one far past that block.
                if (prism && expected >= 0 && brute < fraction) {
                    bool early = false;
                    for (float t = 0; t < brute && !early; t += 4 / length) early = listed(server, from + d * t, server.toIndex(prism));
                    if (early) { ++firstBlock; continue; }
                }
                ++wrong;
                std::fprintf(stderr, "RAY %s: server %s at %.5f, brute force %s at %.5f\n", label.c_str(), prism ? "hit" : "missed",
                             prism ? fraction : 0.0f, expected >= 0 ? "hit" : "missed", expected >= 0 ? brute : 0.0f);
            }
            delete server.mapInfo;
        }
    }
    std::printf("%zu KCL files, %zu loaded, %zu triangles (%zu disabled as degenerate)\n", files, files - failed, triangles, disabled);
    std::printf("%zu without an attribute table, %zu naming missing rows; %zu camera codes collected\n", noAttributes,
                badAttributes, cameraCodes);
    std::printf("%zu rays: %zu hits agree with brute force, %zu differ only at an edge, %zu meet a triangle the disc's "
                "octree does not list there, %zu stop at an earlier block's farther hit, %zu disagree\n", rays, hits, edges,
                unlisted, firstBlock, wrong);
    // Missing attributes are the disc's own (CollisionParts::init ignores the result); only the port can fail here.
    return failed + wrong == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[2], "--gravity")) return auditGravity(argv[1]);
    if (argc == 3 && !std::strcmp(argv[2], "--collision")) return auditCollision(argv[1]);
    const bool models = argc == 3 && !std::strcmp(argv[2], "--models");
    if (argc != 2 && !models) { std::fprintf(stderr,"Usage: resource_audit EXTRACTED_FILES_DIRECTORY [--models|--gravity|--collision]\n"); return 2; }
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
    std::size_t tables=0, records=0, resources=0, errors=0, modelCount=0, triangles=0, materials=0, exact=0;
    std::map<std::string, std::size_t> modelErrors, approximations;
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
        if (!models) continue;
        for (const auto& entry : entries) {
            if (!extension(entry.path,".bdl") && !extension(entry.path,".bmd")) continue;
            ++modelCount;
            smg::Model model;
            if (smg::loadModel({storage.data()+entry.offset,entry.size},model,256u*1024*1024,error)) {
                triangles += model.vertices.size()/3;
                for (const auto& material : model.materials) {
                    ++materials;
                    const auto program = smg::compileTev(material);
                    if (program.exact) ++exact;
                    else ++approximations[program.approximation];
                }
                continue;
            }
            std::fprintf(stderr,"MODEL %s :: %s: %s\n",path.c_str(),entry.path.c_str(),error.c_str());
            ++modelErrors[error.substr(0, error.find(':'))];
        }
    }
    std::printf("%zu archives, %zu resources, %zu BCSV tables, %zu records, %zu errors\n",
        archives.size(),resources,tables,records,errors);
    if (models) {
        std::size_t failed = 0;
        for (const auto& [reason, count] : modelErrors) { std::printf("%6zu  %s\n", count, reason.c_str()); failed += count; }
        std::printf("%zu models, %zu loaded, %zu triangles\n", modelCount, modelCount - failed, triangles);
        std::printf("%zu materials, %zu translated to the 3DS combiner exactly\n", materials, exact);
        for (const auto& [reason, count] : approximations) std::printf("%6zu  approximated: %s\n", count, reason.c_str());
        errors += failed;
    }
    if (archives.empty()) { std::fprintf(stderr,"No archives found\n"); return 2; }
    return errors == 0 ? 0 : 1;
}
