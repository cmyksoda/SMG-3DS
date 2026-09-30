#include <smg/Actors.hpp>
#include <smg/J3DImage.hpp>
#include <Game/Animation/XanimeCore.hpp>
#include <JSystem/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3DGraphAnimator/J3DMtxBuffer.hpp>
#include <JSystem/J3DGraphLoader/J3DModelLoader.hpp>
#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <smg/Bmg.hpp>
#include <smg/Dol.hpp>
#include <smg/NativeResources.hpp>
#include <smg/Animation.hpp>
#include <smg/Combiner.hpp>
#include <smg/Kcl.hpp>
#include <smg/Model.hpp>
#include <smg/Scene.hpp>
#include <smg/Stage.hpp>
#include <smg/StageCollision.hpp>
#include <smg/StageGravity.hpp>
#include <Game/Gravity/GravityInfo.hpp>
#include <Game/Gravity/PlanetGravity.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/Gravity/GravityCreator.hpp>
#include <Game/Gravity/WireGravity.hpp>
#include <Game/LiveActor/RailRider.hpp>
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
int auditGravity(const fs::path& root, bool dropProbes) {
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
                // JMapLinkInfo: a row linked to a map part follows it.
                r.follower += used(info, row, "MapParts_ID") || used(info, row, "Obj_ID") || used(info, row, "ChildObjId");
                any = true;
            }
        }
        zonesWithGravity += any;
    }
    std::size_t global = 0, actors = 0;
    for (const auto& [name, r] : rows) (name.rfind("Global", 0) == 0 ? global : actors) += r.count;
    std::printf("Global gravity rows on the disc: %zu, and %zu rows of other objects named for gravity, in %zu of %zu "
                "stage archives\n", global, actors, zonesWithGravity, archives);
    std::printf("%6s %5s %5s %5s %6s  %s\n", "rows", "SW_A", "SW_B", "SLEEP", "Linked", "name (tables)");
    for (const auto& [name, r] : rows) {
        std::string tables;
        for (const auto& t : r.tables) tables += (tables.empty() ? "" : ", ") + t;
        std::printf("%6zu %5zu %5zu %5zu %6zu  %s (%s)\n", r.count, r.switchA, r.switchB, r.sleep, r.follower, name.c_str(), tables.c_str());
    }

    // Every scenario of every galaxy, as the preview loads it.
    std::vector<std::string> galaxies;
    for (const auto& entry : fs::directory_iterator(root / "StageData")) {
        const std::string name = entry.path().filename().string();
        if (entry.is_directory() && fs::exists(entry.path() / (name + "Scenario.arc"))) galaxies.push_back(name);
    }
    std::sort(galaxies.begin(), galaxies.end());
    std::size_t scenarios = 0, failed = 0, placedTotal = 0, startState = 0, sampled = 0, wires = 0, wirePoints = 0, switched = 0,
                asleep = 0;
    std::map<std::string, std::size_t> placed, skipped, areasSkipped;
    std::size_t areasPlaced = 0;
    std::vector<std::string> noStartGravity, withSkipped, noEntrance, noLanding, farLandings, probeApart;
    std::size_t probes = 0, probesLanded = 0, probesSame = 0;
    float probeDrift = 0;
    std::vector<std::string> probeDrifted;
    std::map<std::string, std::size_t> landings, floors;
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
            for (const auto& g : scene.placedGravities()) {
                switched += g.switches || g.sleep;
                asleep += !g.gravity->mAppeared;
            }
            areasPlaced += scene.areaCount();
            for (const auto& name : scene.skippedAreas())
                if (name.rfind("LightCtrl", 0) == 0) ++areasSkipped[name];
            startState += scene.startStateGravities().size();

            // Each wire's samples lie along its rail from the rail's first point, evenly to within the few percent the
            // original's numerically integrated Bezier lengths allow.
            for (const auto& g : scene.placedGravities()) {
                if (g.name != "GlobalWireGravity") continue;
                const auto* creator = static_cast<const WireGravityCreator*>(g.creator);
                const auto& points = creator->mGravityInstance->mPoints;
                const RailRider* rider = creator->mRailRider;
                ++wires;
                wirePoints += points.size();
                const float interval = rider->getTotalLength() / points.size();
                bool even = points.size() >= 2 && (points[0] - rider->mStartPos).length() < 0.01f;
                float shortest = 1e30f, longest = 0;
                for (int i = 1; i < points.size(); ++i) {
                    const float chord = (points[i] - points[i - 1]).length();
                    shortest = std::min(shortest, chord / interval), longest = std::max(longest, chord / interval);
                    even &= std::isfinite(chord) && chord <= interval * 1.05f && chord >= interval * 0.5f;
                }
                if (even) continue;
                std::fprintf(stderr, "WIRE %s: %d samples of a %.1f rail are not even along it (chords %.3f to %.3f of %.1f)\n",
                             label.c_str(), points.size(), rider->getTotalLength(), shortest, longest, interval);
                ++errors;
            }

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
            else {
                // Follow the start's pull onto the placed models' own collision.
                smg::StageCollision collision;
                smg::StageCollision::Hit hit;
                bool hitFound = false;
                if (!collision.load(root.string(), stage, error)) {
                    std::fprintf(stderr, "COLLISION %s: %s\n", label.c_str(), error.c_str());
                    ++errors;
                } else if (!(hitFound = collision.land(start, pull, hit))) {
                    noLanding.push_back(label);
                } else {
                    ++landings[hit.distance < -1 ? "1 to 100 units above the start" : hit.distance < 0 ? "within 1 unit above the start"
                               : hit.distance < 50 ? "under 50 units below" : hit.distance < 500 ? "50 to 500 units below"
                               : "500 units or more below"];
                    if (hit.distance < -1)
                        std::fprintf(stderr, "ABOVE %s: %s %.1f units above the start, normal (%.2f %.2f %.2f)\n", label.c_str(),
                                     stage.objects[hit.object].name.c_str(), -hit.distance, hit.normal.x, hit.normal.y, hit.normal.z);
                    ++floors[hit.floor];
                    if (hit.distance >= 500) farLandings.push_back(label + " " + stage.objects[hit.object].name);
                }
                if (dropProbes) {
                    // The same fall through the original actor stack: parts as CollisionParts, a ProbeActor, its Binder.
                    collision.addHosts(scene, stage);
                    scene.endActorPlacement();
                    auto* probe = new smg::ProbeActor(TVec3f(start.x - pull.x * 800, start.y - pull.y * 800, start.z - pull.z * 800));
                    for (int frame = 0; frame < 900 && !probe->landed(); ++frame) scene.movement();
                    ++probes;
                    if (probe->landed()) {
                        ++probesLanded;
                        const bool same = hitFound && stage.objects[hit.object].name == probe->groundHost();
                        probesSame += same;
                        if (!same) probeApart.push_back(label + " on " + probe->groundHost());
                        // How far across the pull it came to rest from where the ray met the ground.
                        if (hitFound) {
                            TVec3f apart(probe->mPosition.x - hit.position.x, probe->mPosition.y - hit.position.y,
                                         probe->mPosition.z - hit.position.z);
                            apart -= pull * apart.dot(pull);
                            const float across = apart.length();
                            probeDrift = std::max(probeDrift, across);
                            if (across > 5) probeDrifted.push_back(label + " " + std::to_string(static_cast<int>(across)));
                        }
                    } else {
                        probeApart.push_back(label + " did not land");
                    }
                }
            }
        }
    }
    std::printf("\n%zu galaxies, %zu scenarios, %zu failed to place\n", galaxies.size(), scenarios, failed);
    std::printf("%zu gravities placed across all scenarios, %zu pulls sampled\n", placedTotal, sampled);
    for (const auto& [name, count] : placed) std::printf("%6zu  %s\n", count, name.c_str());
    for (const auto& [name, count] : skipped) std::printf("%6zu  skipped: %s\n", count, name.c_str());
    std::printf("%zu wire gravities sampled their rails into %zu points, from each rail's start, chords within 5%% of even\n",
                wires, wirePoints);
    std::printf("%zu placements follow their switches (%zu asleep as the scene starts); %zu are linked to a map part whose\n"
                "matrix they would follow (followers are not brought up)\n", switched, asleep, startState);
    std::printf("%zu light areas placed across all scenarios; not placed:", areasPlaced);
    for (const auto& [name, count] : areasSkipped) std::printf(" %s x%zu,", name.c_str(), count);
    std::printf("\n");
    std::printf("%zu scenarios skip a gravity:", withSkipped.size());
    for (const auto& label : withSkipped) std::printf(" [%s]", label.c_str());
    std::printf("\n%zu hidden-star scenarios have no MarioNo 0 entrance in their layers:", noEntrance.size());
    for (const auto& label : noEntrance) std::printf(" [%s]", label.c_str());
    std::printf("\n%zu scenarios have no gravity at Mario's start:", noStartGravity.size());
    for (const auto& label : noStartGravity) std::printf(" [%s]", label.c_str());
    std::printf("\nFollowing the start's pull onto the placed models' collision:\n");
    for (const auto& [range, count] : landings) std::printf("%6zu  land %s\n", count, range.c_str());
    for (const auto& [floor, count] : floors) std::printf("%6zu  on a %s floor\n", count, floor.c_str());
    std::printf("%zu land far below:", farLandings.size());
    for (const auto& label : farLandings) std::printf(" [%s]", label.c_str());
    std::printf("\n%zu meet no collision:", noLanding.size());
    for (const auto& label : noLanding) std::printf(" [%s]", label.c_str());
    std::printf("\n");
    if (dropProbes) {
        std::printf("%zu probes dropped 800 up the start's pull as original actors: %zu landed through their Binder, %zu on the "
                    "part the ray found\n", probes, probesLanded, probesSame);
        for (const auto& label : probeApart) std::printf("  [%s]\n", label.c_str());
        std::printf("most rested %.1f units across the pull from the ray's landing; more than 5:", probeDrift);
        for (const auto& label : probeDrifted) std::printf(" [%s]", label.c_str());
        std::printf("\n");
    }
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

// Every BMG on the disc turned native, then read the way MessageData, MessageEditorMessageTag and TalkNodeCtrl
// read it, against the original's big-endian values.
int auditBmg(const fs::path& root) {
    const auto be16 = [](const std::uint8_t* at) { return unsigned(at[0]) << 8 | at[1]; };
    const auto be32 = [&](const std::uint8_t* at) { return std::uint32_t(be16(at)) << 16 | be16(at + 2); };
    const auto n16 = [](const std::uint8_t* at) { std::uint16_t v; std::memcpy(&v, at, 2); return unsigned(v); };
    const auto n32 = [](const std::uint8_t* at) { std::uint32_t v; std::memcpy(&v, at, 4); return v; };
    std::size_t files = 0, messages = 0, units = 0, nodes = 0, wrong = 0, failed = 0;
    std::map<std::pair<unsigned, unsigned>, std::size_t> kinds;
    std::vector<fs::path> archives;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file() && extension(entry.path().string(), ".arc")) archives.push_back(entry.path());
    std::sort(archives.begin(), archives.end());
    for (const auto& path : archives) {
        std::vector<std::uint8_t> storage;
        std::vector<smg::ArchiveEntry> entries;
        std::string error;
        if (!smg::readArchive(path.c_str(), storage, entries, 64u << 20, error)) continue;
        for (const auto& entry : entries) {
            if (!extension(entry.path, ".bmg")) continue;
            ++files;
            const std::vector<std::uint8_t> original(storage.begin() + entry.offset, storage.begin() + entry.offset + entry.size);
            std::vector<std::uint8_t> bmg = original;
            if (const char* why = smg::nativeBmg(bmg.data(), bmg.size())) {
                std::printf("%s %s: %s\n", path.c_str(), entry.path.c_str(), why);
                ++failed;
                continue;
            }
            std::vector<std::uint8_t> again = bmg;
            wrong += smg::nativeBmg(again.data(), again.size()) != nullptr || again != bmg;
            const auto miss = [&](bool bad, const char* what, std::size_t at) {
                if (bad && wrong++ < 20) std::printf("%s %s: %s at 0x%zx\n", path.c_str(), entry.path.c_str(), what, at);
            };
            // Blocks by the header's count, as MessageHolder's getBlock walks them.
            std::size_t inf = 0, dat = 0, flw = 0;
            for (std::uint32_t i = 0, at = 0x20; i < n32(bmg.data() + 0xC); ++i) {
                const std::uint32_t magic = n32(bmg.data() + at);
                miss(magic != be32(original.data() + at) || n32(bmg.data() + at + 4) != be32(original.data() + at + 4), "a block header", at);
                if (magic == 0x494E4631) inf = at;
                if (magic == 0x44415431) dat = at;
                if (magic == 0x464C5731) flw = at;
                at += n32(bmg.data() + at + 4);
            }
            const std::uint8_t *o = original.data(), *n = bmg.data();
            const unsigned count = n16(n + inf + 8), itemSize = n16(n + inf + 10);
            miss(count != be16(o + inf + 8) || itemSize != be16(o + inf + 10), "INF1's counts", inf);
            for (unsigned i = 0; i < count; ++i) {
                const std::size_t item = inf + 16 + i * itemSize;
                miss(n32(n + item) != be32(o + item) || n16(n + item + 4) != be16(o + item + 4) ||
                     std::memcmp(n + item + 6, o + item + 6, itemSize - 6), "an INF1 item", item);
                ++messages;
                for (std::size_t at = dat + 8 + be32(o + item);;) {
                    ++units;
                    if (be16(o + at) != 0x1A) {
                        miss(n16(n + at) != be16(o + at), "a character", at);
                        if (!be16(o + at)) break;
                        at += 2;
                        continue;
                    }
                    const std::uint8_t *t = n + at + 2, *u = o + at + 2;  // as MessageEditorMessageTag holds it
                    const unsigned length = u[0], group = u[1], type = be16(u + 2);
                    ++kinds[{group, type}];
                    miss(n16(n + at) != 0x1A || t[0] != length || t[1] != group || n16(t + 2) != type, "a tag's head", at);
                    if (length == 8 && group == 1 && type == 0) miss(n16(t + 4) != be16(u + 4), "a wait (getParam16)", at);
                    if (group == 2 || group == 10)
                        for (unsigned k = 4; k < length - 2; k += 2) miss(n16(t + k) != be16(u + k), "a name (getParamPtr)", at);
                    if (length == 8 && (group == 5 || group == 255)) miss(t[4] != u[4], "a byte (getParam8)", at);
                    if (length == 14 && (group == 6 || group == 7))
                        miss(n32(t + 4) != 0 || n32(t + 8) != be32(u + 8), "a slot and argument (getParam32)", at);
                    at += length;
                }
            }
            if (flw) {
                const unsigned nodeCount = n16(n + flw + 8), branches = n16(n + flw + 10);
                miss(nodeCount != be16(o + flw + 8) || branches != be16(o + flw + 10), "FLW1's counts", flw);
                for (unsigned i = 0; i < nodeCount; ++i, ++nodes) {
                    const std::size_t node = flw + 16 + i * 8;
                    miss(n[node] != o[node] || n[node + 1] != o[node + 1] || n16(n + node + 2) != be16(o + node + 2), "a node's head", node);
                    if (o[node] == 3) miss(n32(n + node + 4) != be32(o + node + 4), "an event's argument", node);
                    else miss(n16(n + node + 4) != be16(o + node + 4) || n16(n + node + 6) != be16(o + node + 6), "a node's halves", node);
                }
                const std::size_t table = flw + 16 + nodeCount * 8;
                for (unsigned i = 0; i < branches; ++i) miss(n16(n + table + i * 2) != be16(o + table + i * 2), "a branch", table + i * 2);
                miss(std::memcmp(n + table + branches * 2, o + table + branches * 2, branches), "the byte table", table + branches * 2);
            }
        }
    }
    std::printf("%zu BMG files: %zu messages, %zu characters and tags, %zu flow nodes; %zu tag kinds;", files, messages, units, nodes, kinds.size());
    std::printf(" %zu refused, %zu reads differ\n", failed, wrong);
    return failed + wrong == 0 && files ? 0 : 1;
}

int auditLayouts(const fs::path& root);  // tools/AuditLayouts.cpp
int auditAudio(const fs::path& root);  // tools/AuditAudio.cpp

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[2], "--bmg")) return auditBmg(argv[1]);
    if (argc == 3 && !std::strcmp(argv[2], "--layouts")) return auditLayouts(argv[1]);
    if (argc == 3 && !std::strcmp(argv[2], "--audio")) return auditAudio(argv[1]);
    if (argc == 3 && !std::strcmp(argv[2], "--gravity")) return auditGravity(argv[1], false);
    if (argc == 3 && !std::strcmp(argv[2], "--actors")) return auditGravity(argv[1], true);
    if (argc == 3 && !std::strcmp(argv[2], "--collision")) return auditCollision(argv[1]);
    // main.dol's own tables, which the game attaches in place of an archive's.
    if (argc == 3 && !std::strcmp(argv[2], "--dol")) {
        std::vector<std::uint8_t> dol;
        std::FILE* file = std::fopen(argv[1], "rb");
        if (!file) { std::fprintf(stderr,"Cannot open %s\n",argv[1]); return 2; }
        for (int c; (c = std::fgetc(file)) != EOF;) dol.push_back(static_cast<std::uint8_t>(c));
        std::fclose(file);
        smg::DolTables tables; std::string error;
        if (!smg::findDolTables({dol.data(),dol.size()},tables,error)) { std::fprintf(stderr,"%s\n",error.c_str()); return 1; }
        for (auto* table : {&tables.galaxyId, &tables.storyEvent}) {
            JMapInfo info;
            info.attach(table->data());
            const char* first = nullptr;
            info.getValue(0,"name",&first);
            std::printf("%s: %zu bytes, %d rows, first \"%s\"%s\n", table == &tables.galaxyId ? "GalaxyIDBCSV" : "StoryEventBCSV",
                        table->size(), info.getNumEntries(), first ? first : "?",
                        info.mData != reinterpret_cast<const JMapData*>(table->data()) ? " (attached as a repacked copy)" : "");
            if (info.mData != reinterpret_cast<const JMapData*>(table->data())) std::free(const_cast<JMapData*>(info.mData));
        }
        u32 stars = 0;
        JMapInfo galaxies; galaxies.attach(tables.galaxyId.data());
        galaxies.findElement("name","EggStarGalaxy",0).getValue<u32>("PowerStarNum",&stars);
        std::printf("EggStarGalaxy opens at %u power stars\n", stars);
        if (galaxies.mData != reinterpret_cast<const JMapData*>(tables.galaxyId.data())) std::free(const_cast<JMapData*>(galaxies.mData));
        return 0;
    }
    const bool models = argc == 3 && !std::strcmp(argv[2], "--models");
    if (argc != 2 && !models) { std::fprintf(stderr,"Usage: resource_audit EXTRACTED_FILES_DIRECTORY [--models|--gravity|--actors|--collision|--bmg|--layouts|--audio] | MAIN_DOL --dol\n"); return 2; }
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
    std::size_t bcsvInPlace=0, bcsvRepack=0, bcsvDiffer=0;
    std::vector<float> xanimeGaps;
    std::size_t brks=0, brkErrors=0, brkUnmatched=0, skinChecked=0, skinWrong=0, j3dChecked=0, j3dWrong=0, xanimeChecked=0, bcks=0, bckErrors=0;
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
                continue;
            }
            records += table.info().getNumEntries();
            // The game's own loading attaches the disc's bytes: turned native in place, every value must match.
            std::vector<std::uint32_t> disc((entry.size+3)/4);
            std::memcpy(disc.data(),storage.data()+entry.offset,entry.size);
            if (smgNativeBcsvProblem(disc.data())) ++bcsvRepack;
            else ++bcsvInPlace;
            JMapInfo attached;
            attached.attach(disc.data());
            bool same = attached.getNumEntries()==table.info().getNumEntries() && attached.getNumFields()==table.info().getNumFields();
            for (int row=0; same && row<attached.getNumEntries(); ++row) {
                for (int item=0; same && item<attached.getNumFields(); ++item) {
                    const u8 type = attached.mData->mItems[item].mType;
                    if (type==1 || type==6) {
                        const char *a=nullptr, *b=nullptr;
                        same = attached.getValueFast(row,item,&a)==table.info().getValueFast(row,item,&b) && (!a || !b || !std::strcmp(a,b));
                    } else {
                        u32 a=0, b=0;
                        same = attached.getValueFast(row,item,&a)==table.info().getValueFast(row,item,&b) && a==b;
                    }
                }
            }
            if (attached.mData != reinterpret_cast<const JMapData*>(disc.data())) std::free(const_cast<JMapData*>(attached.mData));
            if (!same) { ++bcsvDiffer; std::fprintf(stderr,"BCSV attached %s :: %s: values differ\n",path.c_str(),entry.path.c_str()); }
        }
        if (!models) continue;
        // BRKs, set on the archive's models: every register table should name one of their materials.
        std::vector<smg::Model> archiveModels;
        std::vector<const smg::ArchiveEntry*> archiveModelEntries;
        for (const auto& entry : entries) {
            if (!extension(entry.path,".bdl") && !extension(entry.path,".bmd")) continue;
            smg::Model model;
            if (!smg::loadModel({storage.data()+entry.offset,entry.size},model,256u*1024*1024,error,true)) continue;
            // The skeleton posed at its own bind pose, then skinned, must give back what the loader baked.
            std::vector<smg::JointTransform> bind(model.joints.size());
            for (std::size_t j = 0; j < bind.size(); ++j) {
                const auto& joint = model.joints[j];
                bind[j] = {{joint.scale[0], joint.scale[1], joint.scale[2]}, {joint.rotation[0], joint.rotation[1], joint.rotation[2]},
                           {joint.translation[0], joint.translation[1], joint.translation[2]}};
            }
            std::vector<std::array<float, 12>> world;
            smg::poseSkeleton(model, bind, world);
            std::vector<smg::ModelVertex> skinned;
            smg::skinModel(model, world, skinned);
            float worst = 0;
            for (std::size_t v = 0; v < skinned.size(); ++v)
                for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(skinned[v].position[c] - model.vertices[v].position[c]));
            ++skinChecked;
            if (worst > 0.01f) {
                ++skinWrong;
                std::fprintf(stderr,"SKIN %s :: %s: bind pose off by %g\n",path.c_str(),entry.path.c_str(),worst);
            }
            // The same skeleton through JSystem's own loader and J3DModel: every joint and envelope matrix.
            smg::J3DSkeletonImage image;
            if (!image.load({storage.data()+entry.offset,entry.size},error)) {
                ++j3dWrong;
                std::fprintf(stderr,"J3D %s :: %s: %s\n",path.c_str(),entry.path.c_str(),error.c_str());
            } else {
                J3DModelLoader_v26 loader;
                J3DModel* j3d = new J3DModel(loader.load(image.data(), 0), 0, 1);
                j3d->calcAnmMtx();
                j3d->mMtxBuffer->calcWeightEnvelopeMtx();
                std::vector<std::array<float, 12>> draws;
                smg::skinMatrices(model, world, draws);
                float off = 0;
                const auto compare = [&](const float* a, const std::array<float, 12>& b) {
                    for (int c = 0; c < 12; ++c) off = std::max(off, std::fabs(a[c] - b[c]) / std::max(1.0f, std::fabs(b[c])));
                };
                for (std::size_t j = 0; j < world.size(); ++j) compare(&j3d->getAnmMtx(static_cast<int>(j))[0][0], world[j]);
                for (std::size_t d = 0; d < draws.size(); ++d)
                    if (model.skin.draws[d].weighted) compare(&j3d->getWeightAnmMtx(model.skin.draws[d].index)[0][0], draws[d]);
                ++j3dChecked;
                if (off > 1e-4f) {
                    ++j3dWrong;
                    unsigned compensated = 0;
                    for (u16 j = 0; j < j3d->getModelData()->getJointNum(); ++j)
                        compensated += j3d->getModelData()->getJointNodePointer(j)->getScaleCompensate() != 0;
                    std::fprintf(stderr,"J3D %s :: %s: matrices differ by %g (mode %u, %u of %zu joints scale-compensate)\n",
                                 path.c_str(),entry.path.c_str(),off,j3d->getModelData()->getFlag() & 0xf,compensated,world.size());
                }
                smg::forgetJ3DSkeleton(j3d);
            }
            archiveModels.push_back(std::move(model));
            archiveModelEntries.push_back(&entry);
        }
        for (const auto& entry : entries) {
            if (!extension(entry.path,".bck")) continue;
            ++bcks;
            smg::JointAnimation bck;
            if (!bck.load({storage.data()+entry.offset,entry.size},error)) {
                std::fprintf(stderr,"BCK %s :: %s: %s\n",path.c_str(),entry.path.c_str(),error.c_str()); ++bckErrors; continue;
            }
            std::vector<smg::JointTransform> pose(bck.joints());
            bool finite = true;
            for (int frame = 0; frame <= bck.key().mFrameMax; ++frame) {
                bck.pose(static_cast<float>(frame), pose);
                for (const auto& t : pose) finite &= std::isfinite(t.scale[0] + t.scale[1] + t.scale[2] + t.translation[0] + t.translation[1] + t.translation[2]);
            }
            if (!finite) { ++bckErrors; std::fprintf(stderr,"BCK %s :: %s poses to infinity\n",path.c_str(),entry.path.c_str()); }
            // Through the game's own animation core on the original J3DModel, for the first model it fits.
            for (std::size_t m = 0; m < archiveModels.size(); ++m) {
                const smg::Model& model = archiveModels[m];
                if (model.joints.size() != bck.joints()) continue;
                smg::J3DSkeletonImage image;
                const smg::ArchiveEntry& modelEntry = *archiveModelEntries[m];
                if (!image.load({storage.data()+modelEntry.offset,modelEntry.size},error)) break;
                J3DModelLoader_v26 loader;
                J3DModelData* data = loader.load(image.data(), 0);
                J3DModel* j3d = new J3DModel(data, 0, 1);
                auto* core = new XanimeCore(1, data->getJointNum(), data->getFlag() & 0xf);
                core->initT(data);
                core->setBck(0, &bck.key());
                data->getJointNodePointer(0)->setMtxCalc(core);
                float off = 0;
                const int frames = std::max<int>(1, bck.key().mFrameMax);
                for (int step = 0; step <= 3; ++step) {
                    const float frame = frames * step / 3.0f;
                    core->mFrameRatio = frame / frames;
                    core->updateFrame();
                    j3d->calcAnmMtx();
                    bck.pose(frame, pose);
                    std::vector<std::array<float, 12>> world;
                    smg::poseSkeleton(model, pose, world);
                    for (std::size_t j = 0; j < world.size(); ++j) {
                        const float* original = &j3d->getAnmMtx(static_cast<int>(j))[0][0];
                        for (int c = 0; c < 12; ++c)
                            off = std::max(off, std::fabs(original[c] - world[j][c]) / std::max(1.0f, std::fabs(world[j][c])));
                    }
                }
                ++xanimeChecked;
                xanimeGaps.push_back(off);
                smg::forgetXanimeCore(core);
                smg::forgetJ3DSkeleton(j3d);
                break;
            }
        }
        for (const auto& entry : entries) {
            if (!extension(entry.path,".brk")) continue;
            ++brks;
            smg::TevRegAnimation brk;
            if (!brk.load({storage.data()+entry.offset,entry.size},error)) {
                std::fprintf(stderr,"BRK %s :: %s: %s\n",path.c_str(),entry.path.c_str(),error.c_str()); ++brkErrors; continue;
            }
            unsigned set = 0;
            for (auto& model : archiveModels)
                for (int frame = 0; frame <= brk.key().mFrameMax; ++frame) set += brk.apply(static_cast<float>(frame), model.materials);
            if (!set) { ++brkUnmatched; std::fprintf(stderr,"BRK %s :: %s names no material here\n",path.c_str(),entry.path.c_str()); }
        }
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
    std::printf("BCSV tables attached as the game attaches them: %zu turned native in place, %zu repacked for ARM;"
                " %zu differ from the port's loader\n", bcsvInPlace, bcsvRepack, bcsvDiffer);
    if (models) {
        std::size_t failed = 0;
        for (const auto& [reason, count] : modelErrors) { std::printf("%6zu  %s\n", count, reason.c_str()); failed += count; }
        std::printf("%zu models, %zu loaded, %zu triangles\n", modelCount, modelCount - failed, triangles);
        std::printf("%zu materials, %zu translated to the 3DS combiner exactly\n", materials, exact);
        for (const auto& [reason, count] : approximations) std::printf("%6zu  approximated: %s\n", count, reason.c_str());
        std::printf("%zu BRK files, %zu failed to load, %zu name no material of their archive's models\n", brks, brkErrors, brkUnmatched);
        std::printf("%zu models reposed at their bind pose and skinned, %zu differ from what the loader baked\n", skinChecked, skinWrong);
        std::printf("%zu skeletons through JSystem's J3DModelLoader and J3DModel, %zu differ from the port's matrices\n", j3dChecked, j3dWrong);
        // XanimeCore turns joints through quaternions from half-angle sine lookups, J3D's own calc from full
        // angles; the rounding compounds down long chains. The gap is measured, not an error: actors use XanimeCore.
        std::sort(xanimeGaps.begin(), xanimeGaps.end());
        const auto within = std::count_if(xanimeGaps.begin(), xanimeGaps.end(), [](float v) { return v <= 1e-3f; });
        std::printf("%zu BCKs posed through the game's XanimeCore on J3DModel; the port's Euler pose is within 1e-3 of it on %zu, "
                    "median gap %.4f, worst %.3f\n", xanimeChecked, static_cast<std::size_t>(within),
                    xanimeGaps.empty() ? 0.0f : xanimeGaps[xanimeGaps.size() / 2], xanimeGaps.empty() ? 0.0f : xanimeGaps.back());
        std::printf("%zu BCK files, %zu failed to load or posed to infinity\n", bcks, bckErrors);
        errors += failed + brkErrors + skinWrong + bckErrors;
    }
    if (archives.empty()) { std::fprintf(stderr,"No archives found\n"); return 2; }
    return errors == 0 ? 0 : 1;
}
