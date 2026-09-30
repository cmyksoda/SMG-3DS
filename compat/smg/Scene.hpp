#pragma once
#include <JSystem/JGeometry/TMatrix.hpp>
#include <string>
#include <utility>
#include <vector>

class GravityCreator;
class JMapInfo;
class JMapInfoIter;
class NameObjHolder;
class PlanetGravity;
class SceneObjHolder;
struct JMapData;

namespace smg {
// The scene state GameSystemSceneController owns in the game, held natively until GameSystem runs.
// Removable: see docs/PORTING.md, "The native scene boundary".
class NativeScene {
public:
    NativeScene();
    ~NativeScene();
    NativeScene(const NativeScene&) = delete;
    NativeScene& operator=(const NativeScene&) = delete;

    static NativeScene& current();
    NameObjHolder& nameObjHolder() { return *mNameObjHolder; }
    SceneObjHolder& sceneObjHolder() { return *mSceneObjHolder; }

    // The first half of SceneNameObjMovementController::movement(): apply requested suspends and resumes.
    void movement();
    void requestMovementSync() { mSyncRequested = true; }

    // StageDataHolder's zone placements. Zone 0 is the galaxy; rows are matched to zones by their table.
    int addZone(const TPos3f& placement);
    void addZoneTable(int zone, const JMapInfo& table);
    void forgetZoneTables() { mZoneTables.clear(); }
    bool hasPlacedZones() const { return mZones.size() > 1; }
    int zoneOf(const JMapInfoIter& row) const;
    const TPos3f& zonePlacement(int zone) const { return mZones[zone]; }

    struct PlacedGravity {
        GravityCreator* creator;
        void (*destroy)(GravityCreator*);
        PlanetGravity* gravity;
        std::string name;
        int zone;
    };
    // GlobalGravityObj's work for one planetobjinfo row. False, with the name kept in skippedGravities(),
    // for a kind not brought up.
    bool placeGlobalGravity(const JMapInfoIter& row);
    const std::vector<PlacedGravity>& placedGravities() const { return mGravities; }
    const std::vector<std::string>& skippedGravities() const { return mSkippedGravities; }
    // Rows whose switches or followers are only honored as a scene starts: every switch off, nothing moved.
    const std::vector<std::string>& startStateGravities() const { return mStartStateGravities; }

private:
    NameObjHolder* mNameObjHolder;
    SceneObjHolder* mSceneObjHolder;
    bool mSyncRequested = false;
    std::vector<TPos3f> mZones;
    std::vector<std::pair<const JMapData*, int>> mZoneTables;
    std::vector<PlacedGravity> mGravities;
    std::vector<std::string> mSkippedGravities, mStartStateGravities;
};
} // namespace smg
