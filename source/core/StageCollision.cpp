#include <smg/StageCollision.hpp>
#include <smg/Archive.hpp>
#include <smg/Scene.hpp>
#include <Game/Camera/CameraPolygonCodeUtil.hpp>
#include <Game/Map/CollisionCode.hpp>
#include <Game/Map/KCollision.hpp>
#include <Game/Util/StringUtil.hpp>
#include <algorithm>
#include <cmath>

namespace smg {
namespace {
constexpr std::size_t ArchiveBudget = 32u * 1024 * 1024;

bool endsWith(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() && MR::isEqualStringCase(text.c_str() + text.size() - tail.size(), tail.c_str());
}
} // namespace

StageCollision::~StageCollision() {
    // KCollisionServer has no destructor; the game frees the scene heap whole.
    for (Part& part : parts_) {
        delete part.server->mapInfo;
        delete part.server;
    }
}

std::unique_ptr<StageCollision::Model> StageCollision::readModel(const std::string& name, const std::uint8_t* storage,
                                                                const std::vector<ArchiveEntry>& entries, std::string& error) {
    auto model = std::make_unique<Model>();
    model->name = name;
    for (const ArchiveEntry& entry : entries) {
        const Bytes bytes{storage + entry.offset, entry.size};
        if (endsWith(entry.path, name + ".kcl") && !model->kcl.load(bytes, error)) {
            error = name + ".kcl: " + error;
            return nullptr;
        }
        if (endsWith(entry.path, name + ".pa") && !model->attributes.load(bytes, error)) {
            error = name + ".pa: " + error;
            return nullptr;
        }
    }
    error.clear();
    return model->kcl.triangles() ? std::move(model) : nullptr;
}

void StageCollision::addParts(const Stage& stage, std::unique_ptr<Model> model) {
    for (std::size_t i = 0; i < stage.objects.size(); ++i) {
        const StageObject& object = stage.objects[i];
        if (object.name != model->name) continue;
        Part part{i, model.get(), new KCollisionServer, {}, {}, 0};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 4; ++column) part.base.mMtx[row][column] = object.transform[row * 4 + column];
        PSMTXInverse(part.base.toMtxPtr(), part.inverse.toMtxPtr());
        // CollisionParts::init: attach, then collect the camera codes while the server sets itself up.
        part.server->init(model->kcl.data(), model->attributes.info().mData);
        const auto zone = std::find_if(stage.zones.begin(), stage.zones.end(), [&](const StageZone& z) { return z.name == object.zone; });
        MR::initCameraCodeCollection(object.name.c_str(), static_cast<s32>(zone - stage.zones.begin()));
        part.server->calcFarthestVertexDistance();
        MR::termCameraCodeCollection();
        float scale = 0;
        for (int column = 0; column < 3; ++column) {
            const float length = std::sqrt(part.base.mMtx[0][column] * part.base.mMtx[0][column] +
                                           part.base.mMtx[1][column] * part.base.mMtx[1][column] +
                                           part.base.mMtx[2][column] * part.base.mMtx[2][column]);
            scale = std::max(scale, length);
        }
        part.radius = part.server->mMaxVertexDistance * scale;
        parts_.push_back(part);
    }
    models_.push_back(std::move(model));
}

bool StageCollision::load(const std::string& root, const Stage& stage, std::string& error) {
    std::vector<std::string> names;
    for (const StageObject& object : stage.objects)
        if (std::find(names.begin(), names.end(), object.name) == names.end()) names.push_back(object.name);
    for (const std::string& name : names) {
        std::vector<std::uint8_t> storage;
        std::vector<ArchiveEntry> entries;
        if (!readArchive((root + "/ObjectData/" + name + ".arc").c_str(), storage, entries, ArchiveBudget, error)) continue;
        auto model = readModel(name, storage.data(), entries, error);
        if (!error.empty()) return false;
        if (model) addParts(stage, std::move(model));
    }
    error.clear();
    return true;
}

std::size_t StageCollision::triangles() const {
    std::size_t count = 0;
    for (const auto& model : models_) count += model->kcl.triangles();
    return count;
}

bool StageCollision::strikeLine(const TVec3f& from, const TVec3f& segment, Hit& hit) const {
    const float length = segment.length();
    bool found = false;
    for (const Part& part : parts_) {
        // CollisionCategorizedKeeper skips parts whose bounding sphere the segment misses.
        const TVec3f center(part.base.mMtx[0][3], part.base.mMtx[1][3], part.base.mMtx[2][3]);
        const TVec3f toCenter = center - from;
        const float along = std::clamp(length > 0 ? toCenter.dot(segment) / (length * length) : 0.0f, 0.0f, 1.0f);
        if ((toCenter - segment * along).length() > part.radius) continue;

        // CollisionParts::checkStrikeLine, keeping only the nearest hit.
        TVec3f localFrom, localTo;
        part.inverse.mult(from, localFrom);
        part.inverse.mult(from + segment, localTo);
        const TVec3f localSegment = localTo - localFrom;
        f32 fraction = 0;
        u8 flag = 0;
        const KC_PrismData* prism = part.server->checkArrow(localFrom, localSegment, &fraction, &flag, nullptr, nullptr, 0);
        if (!prism || (found && length * fraction >= hit.distance)) continue;
        found = true;
        hit.object = part.object;
        hit.distance = length * fraction;
        part.base.mult(localFrom + localSegment * fraction, hit.position);
        TVec3f normal = *part.server->getFaceNormal(prism);
        // Normals go through the inverse transpose, so a scaled part keeps them perpendicular.
        hit.normal.set(part.inverse.mMtx[0][0] * normal.x + part.inverse.mMtx[1][0] * normal.y + part.inverse.mMtx[2][0] * normal.z,
                       part.inverse.mMtx[0][1] * normal.x + part.inverse.mMtx[1][1] * normal.y + part.inverse.mMtx[2][1] * normal.z,
                       part.inverse.mMtx[0][2] * normal.x + part.inverse.mMtx[1][2] * normal.y + part.inverse.mMtx[2][2] * normal.z);
        hit.normal.normalize();
        CollisionCode& code = NativeScene::current().collisionCode();
        hit.floorCode = code.getFloorCode(part.server->getAttributes(part.server->toIndex(prism)));
        hit.floor = code.mFloorTable->getString(hit.floorCode);
    }
    return found;
}
} // namespace smg
