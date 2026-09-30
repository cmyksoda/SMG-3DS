#include <smg/Archive.hpp>
#include <smg/Model.hpp>
#include <smg/Scene.hpp>
#include <smg/StageCollision.hpp>
#include <smg/StageGravity.hpp>
#include <Game/Gravity/GravityInfo.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <Game/Util/StringUtil.hpp>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <map>

namespace {
// Archive entries are stored in lowercase.
bool endsWith(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() && MR::isEqualStringCase(text.c_str() + text.size() - tail.size(), tail.c_str());
}

// Distance along a unit ray to a triangle (Moller-Trumbore), or a negative number for a miss.
float hit(const float o[3], const float d[3], const float a[3], const float b[3], const float c[3]) {
    const float e1[3]{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3]{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float p[3]{d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-8f) return -1;
    const float t0[3]{o[0] - a[0], o[1] - a[1], o[2] - a[2]};
    const float u = (t0[0] * p[0] + t0[1] * p[1] + t0[2] * p[2]) / det;
    const float q[3]{t0[1] * e1[2] - t0[2] * e1[1], t0[2] * e1[0] - t0[0] * e1[2], t0[0] * e1[1] - t0[1] * e1[0]};
    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
    if (u < 0 || v < 0 || u + v > 1) return -1;
    return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
}

// Follows the pull from a point to the first placed model surface it meets, as a check of "down".
void fall(const std::string& root, const smg::Stage& stage, const TVec3f& at, const TVec3f& pull) {
    const float o[3]{at.x, at.y, at.z}, d[3]{pull.x, pull.y, pull.z};
    float nearest = 1e30f;
    const smg::StageObject* landing = nullptr;
    std::map<std::string, smg::Model> models;
    for (const auto& object : stage.objects) {
        auto found = models.find(object.name);
        if (found == models.end()) {
            smg::Model model;
            std::vector<std::uint8_t> storage;
            std::vector<smg::ArchiveEntry> entries;
            std::string error;
            if (smg::readArchive((root + "/ObjectData/" + object.name + ".arc").c_str(), storage, entries, 64u << 20, error)) {
                for (const auto& entry : entries) {
                    if (!endsWith(entry.path, object.name + ".bdl") && !endsWith(entry.path, object.name + ".bmd")) continue;
                    smg::loadModel({storage.data() + entry.offset, entry.size}, model, 64u << 20, error);
                    break;
                }
            }
            found = models.emplace(object.name, std::move(model)).first;
        }
        const auto& m = object.transform;
        const auto& vertices = found->second.vertices;
        for (std::size_t i = 0; i + 2 < vertices.size(); i += 3) {
            float w[3][3];
            for (int k = 0; k < 3; ++k) {
                const float* v = vertices[i + k].position;
                for (int r = 0; r < 3; ++r) w[k][r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3];
            }
            const float t = hit(o, d, w[0], w[1], w[2]);
            if (t > 0 && t < nearest) nearest = t, landing = &object;
        }
    }
    if (landing)
        std::printf("Falling along it meets %s (%s) after %.1f units.\n", landing->name.c_str(), landing->zone.c_str(), nearest);
    else
        std::printf("Falling along it meets no placed model.\n");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 7) {
        std::fprintf(stderr, "Usage: gravity_tool EXTRACTED_FILES_DIRECTORY GALAXY SCENARIO [X Y Z]\n");
        return 2;
    }
    smg::Stage stage;
    std::string error;
    if (!smg::loadStage(argv[1], argv[2], std::atoi(argv[3]), stage, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    smg::NativeScene scene;
    if (!smg::placeStageGravity(argv[1], stage, scene, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::map<std::string, int> placed, skipped;
    for (const auto& gravity : scene.placedGravities()) ++placed[gravity.name];
    for (const auto& name : scene.skippedGravities()) ++skipped[name];
    std::printf("%zu gravities placed in %zu zones\n", scene.placedGravities().size(), stage.zones.size());
    for (const auto& [name, count] : placed) std::printf("%5d %s\n", count, name.c_str());
    for (const auto& [name, count] : skipped) std::printf("%5d %s (not brought up)\n", count, name.c_str());
    std::printf("%zu use switches, sleep or a follower and keep their start state\n", scene.startStateGravities().size());

    const TVec3f at = argc == 7 ? TVec3f(std::atof(argv[4]), std::atof(argv[5]), std::atof(argv[6]))
                                : TVec3f(stage.start[0], stage.start[1], stage.start[2]);
    std::printf("\nAt (%g, %g, %g)%s:\n", at.x, at.y, at.z,
                argc == 7 ? "" : stage.entrance ? ", the scenario's start" : " (the scenario has no MarioNo 0 entrance)");
    for (const auto& gravity : scene.placedGravities()) {
        TVec3f pull;
        if (!gravity.gravity->mActivated || !gravity.gravity->calcGravity(&pull, at)) continue;
        std::printf("  in range: %-28s %-22s priority %3d  pull (%.3f, %.3f, %.3f) strength %.3f\n", gravity.name.c_str(),
                    stage.zones[gravity.zone].name.c_str(), gravity.gravity->mPriority, pull.x, pull.y, pull.z, pull.length());
    }
    TVec3f pull;
    GravityInfo info;
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    if (!manager->calcTotalGravityVector(&pull, &info, at, GRAVITY_TYPE_NORMAL, 0)) {
        std::printf("No gravity here.\n");
        return 0;
    }
    std::printf("Total pull (%.4f, %.4f, %.4f)", pull.x, pull.y, pull.z);
    for (const auto& gravity : scene.placedGravities()) {
        if (gravity.gravity == info.mGravityInstance)
            std::printf(", strongest from %s in %s at priority %d", gravity.name.c_str(), stage.zones[gravity.zone].name.c_str(),
                        info.mLargestPriority);
    }
    std::printf("\n");
    fall(argv[1], stage, at, pull);

    smg::StageCollision collision;
    if (!collision.load(argv[1], stage, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("Collision: %zu parts, %zu triangles from the placed models' own KCL files\n", collision.parts(),
                collision.triangles());
    smg::StageCollision::Hit hit;
    if (collision.land(at, pull, hit)) {
        const auto& object = stage.objects[hit.object];
        std::printf("Falling along it lands on %s's collision (%s) after %.1f units, at (%.1f, %.1f, %.1f), face normal "
                    "(%.3f, %.3f, %.3f), floor code %s.\n", object.name.c_str(), object.zone.c_str(), hit.distance,
                    hit.position.x, hit.position.y, hit.position.z, hit.normal.x, hit.normal.y, hit.normal.z, hit.floor);
    } else {
        std::printf("Falling along it meets no collision.\n");
    }
    return 0;
}
