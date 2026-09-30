#pragma once
#include <JSystem/JGeometry/TMatrix.hpp>
#include <memory>
#include <revolution/types.h>
#include <string>
#include <utility>
#include <vector>

struct AreaLightInfo;
class CollisionCode;
class LightDataHolder;
class LightZoneDataHolder;
class GravityCreator;
class SleepController;
class StageSwitchCtrl;
class JMapInfo;
class JMapInfoIter;
class NameObjHolder;
class PlanetGravity;
class SceneObjHolder;
struct JMapData;

namespace smg {
class Bcsv;

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
    int addZone(const std::string& name, const TPos3f& placement);
    int zoneCount() const { return static_cast<int>(mZones.size()); }
    const std::string& zoneName(int zone) const { return mZoneNames[zone]; }
    void addZoneTable(int zone, const JMapInfo& table);
    // A zone's CommonPathInfo and its CommonPathPointInfo.<row> tables, which StageDataHolder keeps for rails.
    void addZonePaths(int zone, const JMapInfo& paths, std::vector<const JMapInfo*> points);
    void forgetZoneTables() { mZoneTables.clear(); mZonePaths.clear(); }
    bool hasPlacedZones() const { return mZones.size() > 1; }
    int zoneOf(const JMapInfoIter& row) const;
    const TPos3f& zonePlacement(int zone) const { return mZones[zone]; }
    struct ZonePaths {
        int zone;
        const JMapInfo* paths;
        std::vector<const JMapInfo*> points;
    };
    const ZonePaths* zonePaths(int zone) const;

    struct GravitySleep;
    struct PlacedGravity {
        GravityCreator* creator;
        void (*destroy)(GravityCreator*);
        PlanetGravity* gravity;
        std::string name;
        int zone;
        StageSwitchCtrl* switches; // SW_A and SW_B, as the actor's StageSwitchCtrl holds them
        GravitySleep* sleep;       // SW_SLEEP
    };
    // GlobalGravityObj's work for one planetobjinfo row. False, with the name kept in skippedGravities(),
    // for a kind not brought up.
    bool placeGlobalGravity(const JMapInfoIter& row);
    // GameScene::init's last step after placement: every sleep controller syncs with its switch.
    void syncAfterPlacement();
    const std::vector<PlacedGravity>& placedGravities() const { return mGravities; }
    const std::vector<std::string>& skippedGravities() const { return mSkippedGravities; }
    // Rows with a follower (FollowId), which keep their placement: base matrix followers are not brought up.
    const std::vector<std::string>& startStateGravities() const { return mStartStateGravities; }

    // LightDirector's data: LightData.bcsv and each zone's Light<zone>.bcsv, which the original holders read.
    // The tables are loaded outside the scene and handed over; initLights runs the holders' own init.
    void initLights(std::unique_ptr<smg::Bcsv> data, std::vector<std::pair<std::string, std::unique_ptr<smg::Bcsv>>> zones);
    const JMapInfo* lightTable(const char* zone) const; // null for the light data itself, or a zone without a table
    const JMapInfo* lightData() const;
    LightDataHolder* lightDataHolder() const { return mLightData; }
    // LightFunction::getAreaLightInfo for an actor outside every light area: the galaxy's default light.
    const AreaLightInfo* defaultAreaLight() const;

    // CollisionDirector's attribute codes, which KCollisionServer reads camera ids through.
    CollisionCode& collisionCode() { return *mCollisionCode; }
    // What CollisionParts hands CameraDirector while a part's collision initializes, kept until the camera runs.
    struct CameraCodes {
        std::string host;
        s32 zone;
        std::vector<u32> codes;
    };
    void beginCameraCodes(const char* host, s32 zone);
    void addCameraCode(u32 code);
    void endCameraCodes();
    const std::vector<CameraCodes>& cameraCodes() const { return mCameraCodes; }

private:
    void moveGravities();
    void forgetLights();
    static void forgetGravity(const PlacedGravity& placed);

    NameObjHolder* mNameObjHolder;
    SceneObjHolder* mSceneObjHolder;
    bool mSyncRequested = false;
    std::vector<TPos3f> mZones;
    std::vector<std::string> mZoneNames;
    std::unique_ptr<smg::Bcsv> mLightTable;
    std::vector<std::pair<std::string, std::unique_ptr<smg::Bcsv>>> mZoneLightTables;
    LightDataHolder* mLightData = nullptr;
    LightZoneDataHolder* mLightZones = nullptr;
    std::vector<std::pair<const JMapData*, int>> mZoneTables;
    std::vector<ZonePaths> mZonePaths;
    std::vector<PlacedGravity> mGravities;
    std::vector<std::string> mSkippedGravities, mStartStateGravities;
    CollisionCode* mCollisionCode;
    std::vector<CameraCodes> mCameraCodes;
    bool mCollectingCameraCodes = false;
};
} // namespace smg
