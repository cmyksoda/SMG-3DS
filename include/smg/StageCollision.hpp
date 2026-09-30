#pragma once
#include <smg/Archive.hpp>
#include <smg/Bcsv.hpp>
#include <smg/Kcl.hpp>
#include <smg/Stage.hpp>
#include <JSystem/JGeometry/TMatrix.hpp>
#include <memory>
#include <string>
#include <vector>

class KCollisionServer;

namespace smg {
// Each placed model's own collision (<name>.kcl and <name>.pa in its archive) through the original
// KCollisionServer, one server per placement sharing the model's image, as CollisionParts are. Until
// actors run, strikeLine does for every part what CollisionParts::checkStrikeLine does. Needs a NativeScene.
class StageCollision {
public:
    StageCollision() = default;
    StageCollision(const StageCollision&) = delete;
    StageCollision& operator=(const StageCollision&) = delete;
    ~StageCollision();

    struct Model {
        std::string name;
        Kcl kcl;
        Bcsv attributes;
    };
    // A model's <name>.kcl and <name>.pa from its opened archive, or null without them. Touches no scene,
    // so a loader thread can do it.
    static std::unique_ptr<Model> readModel(const std::string& name, const std::uint8_t* storage,
                                            const std::vector<ArchiveEntry>& entries, std::string& error);
    // A part for each placement of the model, set up as CollisionParts::init sets one up.
    void addParts(const Stage& stage, std::unique_ptr<Model> model);
    // Both, reading each placed model's archive.
    bool load(const std::string& root, const Stage& stage, std::string& error);
    struct Hit {
        std::size_t object = 0; // index into Stage::objects
        float distance = 0;
        TVec3f position, normal;
        u32 floorCode = 0;
        const char* floor = "";
    };
    // The nearest face met along from + segment, front faces only.
    bool strikeLine(const TVec3f& from, const TVec3f& segment, Hit& hit) const;
    std::size_t parts() const { return parts_.size(); }
    std::size_t triangles() const;

private:
    struct Part {
        std::size_t object;
        const Model* model;
        KCollisionServer* server;
        TPos3f base, inverse;
        float radius;
    };
    std::vector<std::unique_ptr<Model>> models_;
    std::vector<Part> parts_;
};
} // namespace smg
