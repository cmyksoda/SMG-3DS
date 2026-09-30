#pragma once
#include <JSystem/JGeometry/TMatrix.hpp>
#include <memory>
#include <revolution/types.h>
#include <string>
#include <utility>
#include <vector>

struct AreaLightInfo;
class AreaObj;
class AreaObjMgr;
class CollisionCode;
class LightAreaHolder;
namespace MR {
class FunctorBase;
}
class LightDataHolder;
class LightZoneDataHolder;
class GravityCreator;
class SleepController;
class StageSwitchCtrl;
class SwitchIdInfo;
class JMapInfo;
class JMapInfoIter;
class NameObj;
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
    // NameObjFactory's area objects for one areaobjinfo row, through AreaObj::init. False, with the name kept
    // in skippedAreas(), for a kind not brought up (only light areas are).
    bool placeAreaObj(const JMapInfoIter& row);
    const std::vector<std::string>& skippedAreas() const { return mSkippedAreas; }
    std::size_t areaCount() const { return mAreas.size(); }
    // AreaObjContainer's light area manager, as MR::getAreaObjManager finds it; the holder registers itself.
    LightAreaHolder* lightAreas(bool create);
    void registerLightAreas(LightAreaHolder* holder) { mLightAreas = holder; }
    AreaObjMgr* viewCubes();
    // SwitchWatcherHolder's watch on a switch-appear switch, and SleepControllerHolder's control of an area.
    void watchSwitchAppear(const StageSwitchCtrl* switches, const MR::FunctorBase& on, const MR::FunctorBase& off);
    void addAreaSleep(AreaObj* area, const JMapInfoIter& row);
    // GameScene::init's last steps after placement: every NameObj's initAfterPlacement (the light areas sort
    // by priority), then every sleep controller syncs with its switch.
    void syncAfterPlacement();
    const std::vector<PlacedGravity>& placedGravities() const { return mGravities; }
    const std::vector<std::string>& skippedGravities() const { return mSkippedGravities; }
    // Rows linked to a map part (MapParts_ID, Obj_ID or ChildObjId), whose matrix they follow in the game;
    // base matrix followers are not brought up, so they keep their placement.
    const std::vector<std::string>& startStateGravities() const { return mStartStateGravities; }

    // LightDirector's data: LightData.bcsv and each zone's Light<zone>.bcsv, which the original holders read.
    // The tables are loaded outside the scene and handed over; initLights runs the holders' own init.
    void initLights(std::unique_ptr<smg::Bcsv> data, std::vector<std::pair<std::string, std::unique_ptr<smg::Bcsv>>> zones);
    const JMapInfo* lightTable(const char* zone) const; // null for the light data itself, or a zone without a table
    const JMapInfo* lightData() const;
    LightDataHolder* lightDataHolder() const { return mLightData; }
    // LightFunction::getAreaLightInfo for an actor outside every light area: the galaxy's default light.
    const AreaLightInfo* defaultAreaLight() const;
    // The light an actor at a point takes: ActorLightCtrl::init's area search, then getAreaLightInfo.
    const AreaLightInfo* areaLightAt(const TVec3f& position);

    // CollisionDirector's attribute codes, which KCollisionServer reads camera ids through.
    CollisionCode& collisionCode();

    // NameObjExecuteHolder until the scheduler runs: what connectToScene registered, and whether its movement
    // and draw are connected. movement() runs connected objects' movement, then their calcAnim, in the order
    // they registered; the game's NameObjListExecutor runs them by category, and connects on the next frame.
    void registerExecuted(NameObj* object, int movementType, int calcAnimType, int drawBufferType, int drawType);
    void connectExecuted(NameObj* object, bool movement, bool connected);
    bool isExecuted(const NameObj* object) const;
    // CameraDirector's view, which clipping reads, until the camera runs: the preview's camera once it sets one.
    struct View {
        TPos3f view;
        f32 fovy = 45, aspect = 5 / 3.0f, farZ = 100000;
    };
    void setView(const View& view) { mView = view; }
    const View& view() const { return mView; }
    // GameScene::init's MR::endInitLiveActorSystemInfo, once the last placed actor exists: clipping gives
    // every actor its view group. Actors made later are not judged until they are given one.
    void endActorPlacement();
    // StageDataHolder's zone for the placement in progress; the galaxy's outside placement.
    void setPlacementZone(s32 zone) { mPlacementZone = zone; }
    s32 placementZone() const { return mPlacementZone; }
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
    void moveAreas();
    void syncAreaSleeps();
    void forgetLights();
    void forgetAreas();
    void forgetActors();
    struct AppearWatch;
    struct AreaSleep;
    static void forgetGravity(const PlacedGravity& placed);
    static void forgetSwitches(StageSwitchCtrl* switches);
    static void forgetSwitchId(SwitchIdInfo* info);

    NameObjHolder* mNameObjHolder;
    SceneObjHolder* mSceneObjHolder;
    bool mSyncRequested = false;
    std::vector<TPos3f> mZones;
    std::vector<std::string> mZoneNames;
    std::unique_ptr<smg::Bcsv> mLightTable;
    std::vector<std::pair<std::string, std::unique_ptr<smg::Bcsv>>> mZoneLightTables;
    std::vector<AreaObj*> mAreas;
    std::vector<std::string> mSkippedAreas;
    LightAreaHolder* mLightAreas = nullptr;
    AreaObjMgr* mViewCubes = nullptr;
    std::vector<AppearWatch*> mAppearWatches;
    std::vector<AreaSleep*> mAreaSleeps;
    LightDataHolder* mLightData = nullptr;
    LightZoneDataHolder* mLightZones = nullptr;
    std::vector<std::pair<const JMapData*, int>> mZoneTables;
    std::vector<ZonePaths> mZonePaths;
    std::vector<PlacedGravity> mGravities;
    std::vector<std::string> mSkippedGravities, mStartStateGravities;
    struct Executed {
        NameObj* object;
        int movementType, calcAnimType, drawBufferType, drawType;
        bool moving, drawn;
    };
    std::vector<Executed> mExecuted;
    View mView;
    s32 mPlacementZone = 0;
    std::vector<CameraCodes> mCameraCodes;
    bool mCollectingCameraCodes = false;
};
} // namespace smg
