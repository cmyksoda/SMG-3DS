#include <smg/Bcsv.hpp>
#include <smg/Scene.hpp>
#include <Game/Gravity/ConeGravity.hpp>
#include <Game/Gravity/CubeGravity.hpp>
#include <Game/Gravity/DiskTorusGravity.hpp>
#include <Game/Gravity/GravityInfo.hpp>
#include <Game/Gravity/PlanetGravityManager.hpp>
#include <Game/Gravity/PointGravity.hpp>
#include <Game/Map/LightDataHolder.hpp>
#include <Game/Map/StageSwitch.hpp>
#include <Game/Util/JMapIdInfo.hpp>
#include <Game/NameObj/NameObjHolder.hpp>
#include <Game/Scene/SceneObjHolder.hpp>
#include <Game/Util/MtxUtil.hpp>
#include <JSystem/JGadget/hashcode.hpp>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0;
void check(bool pass, const char* message) {
    ++checks;
    if (!pass) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }
bool vec(const TVec3f& v, float x, float y, float z) { return near(v.x, x) && near(v.y, y) && near(v.z, z); }

void place(PointGravity& gravity, float x, float y, float z, s32 priority) {
    gravity.setOrig(TVec3f(x, y, z));
    gravity.setPriority(priority);
    gravity.updateIdentityMtx();
}

void gravityManager(PlanetGravityManager& manager) {
    TVec3f pull(9.0f, 9.0f, 9.0f);
    GravityInfo info;
    check(!manager.calcTotalGravityVector(&pull, &info, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0), "no gravity, no pull");
    check(vec(pull, 0, 0, 0) && info.mGravityInstance == nullptr, "an empty result is zero");

    PointGravity below, beside, near;
    place(below, 0, -2000, 0, 0);
    place(beside, 2000, 0, 0, 0);
    manager.registerGravity(&below);
    manager.registerGravity(&beside);
    check(below.mIsRegistered && beside.mIsRegistered, "registering marks each gravity");
    check(manager.calcTotalGravityVector(&pull, &info, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0), "two planets pull");
    const float half = std::sqrt(0.5f);
    check(vec(pull, half, -half, 0), "equal priorities add, then normalize");
    check(info.mGravityInstance == &below && info.mLargestPriority == 0, "ties keep the first registered as the info");

    // Registered after the others, but its priority sorts it first.
    place(near, 0, 0, 1000, 10);
    near.mRange = 2000.0f;
    manager.registerGravity(&near);
    check(manager.calcTotalGravityVector(&pull, &info, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0), "the nearby planet pulls");
    check(vec(pull, 0, 0, 1) && info.mGravityInstance == &near && info.mLargestPriority == 10,
          "a higher priority replaces lower ones");
    check(manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, -5000), GRAVITY_TYPE_NORMAL, 0), "out of its range");
    check(!near.calcGravity(&pull, TVec3f(0, 0, -5000)), "the nearby planet's range ends");

    check(manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL,
                                         static_cast<u32>(reinterpret_cast<std::uintptr_t>(near.mHost))),
          "a requester still feels other gravity");
    check(vec(pull, half, -half, 0), "a requester is not pulled by gravity it hosts");

    near.mGravityType = GRAVITY_TYPE_SHADOW;
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0);
    check(vec(pull, half, -half, 0), "a shadow-only gravity leaves normal gravity alone");
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL | GRAVITY_TYPE_SHADOW, 0);
    check(vec(pull, 0, 0, 1), "and counts when shadows ask");
    near.mGravityType = GRAVITY_TYPE_NORMAL;

    near.mActivated = false;
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, 0), GRAVITY_TYPE_NORMAL, 0);
    check(vec(pull, half, -half, 0), "a switched-off gravity is skipped");
    near.mActivated = true;
    beside.mAppeared = false;
    manager.calcTotalGravityVector(&pull, nullptr, TVec3f(0, 0, -5000), GRAVITY_TYPE_NORMAL, 0);
    const float d = std::sqrt(2000.0f * 2000.0f + 5000.0f * 5000.0f);
    check(vec(pull, 0, -2000.0f / d, 5000.0f / d), "a dead gravity is skipped");
}

// A planetobjinfo row, with the fields GlobalGravityObj and GravityCreator read.
struct Row {
    const char* name;
    float pos[3]{}, dir[3]{}, scale[3]{1, 1, 1};
    s32 args[3]{-1, -1, -1};
    s32 priority = 0, switchA = -1, switchB = -1, sleep = -1, rail = -1;
    float range = -1, distant = 0;
    const char* type = "Normal";
    const char* power = "Normal";
    s32 inverse = -1;
};

// The big-endian BCSV the disc stores, so rows go through the same loader as the game's.
std::vector<std::uint8_t> planetTable(const std::vector<Row>& rows) {
    struct Field { const char* name; unsigned type; };
    const Field fields[]{{"name", 6}, {"pos_x", 2}, {"pos_y", 2}, {"pos_z", 2}, {"dir_x", 2}, {"dir_y", 2}, {"dir_z", 2},
                         {"scale_x", 2}, {"scale_y", 2}, {"scale_z", 2}, {"Obj_arg0", 0}, {"Obj_arg1", 0}, {"Obj_arg2", 0},
                         {"Priority", 0}, {"SW_A", 0}, {"Range", 2}, {"Distant", 2}, {"Gravity_type", 6}, {"Power", 6},
                         {"Inverse", 0}, {"CommonPath_ID", 0}, {"SW_B", 0}, {"SW_SLEEP", 0}};
    constexpr unsigned count = sizeof(fields) / sizeof(fields[0]), stride = count * 4, data = 16 + count * 12;
    std::vector<std::uint8_t> bytes(data + rows.size() * stride);
    std::string strings;
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[at + i] = value >> (24 - 8 * i);
    };
    put(0, rows.size()), put(4, count), put(8, data), put(12, stride);
    for (unsigned f = 0; f < count; ++f) {
        put(16 + f * 12, JGadget::getHashCode(fields[f].name)), put(20 + f * 12, 0xffffffff);
        bytes[24 + f * 12] = (f * 4) >> 8, bytes[25 + f * 12] = f * 4, bytes[27 + f * 12] = fields[f].type;
    }
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const Row& row = rows[r];
        const auto text = [&](const char* value) {
            const auto offset = strings.size();
            strings.append(value).push_back('\0');
            return static_cast<std::uint32_t>(offset);
        };
        const auto real = [](float value) { std::uint32_t bits; std::memcpy(&bits, &value, 4); return bits; };
        const std::uint32_t values[]{text(row.name), real(row.pos[0]), real(row.pos[1]), real(row.pos[2]), real(row.dir[0]),
                                     real(row.dir[1]), real(row.dir[2]), real(row.scale[0]), real(row.scale[1]),
                                     real(row.scale[2]), static_cast<std::uint32_t>(row.args[0]),
                                     static_cast<std::uint32_t>(row.args[1]), static_cast<std::uint32_t>(row.args[2]),
                                     static_cast<std::uint32_t>(row.priority), static_cast<std::uint32_t>(row.switchA),
                                     real(row.range), real(row.distant), text(row.type), text(row.power),
                                     static_cast<std::uint32_t>(row.inverse), static_cast<std::uint32_t>(row.rail),
                                     static_cast<std::uint32_t>(row.switchB), static_cast<std::uint32_t>(row.sleep)};
        for (unsigned f = 0; f < count; ++f) put(data + r * stride + f * 4, values[f]);
    }
    bytes.insert(bytes.end(), strings.begin(), strings.end());
    return bytes;
}

// A zone's CommonPathInfo with one open rail, and its point table: straight spans (0,0,0), (1000,0,0), (1000,0,1000).
std::vector<std::uint8_t> bigEndianTable(const std::vector<std::pair<const char*, unsigned>>& fields,
                                         const std::vector<std::vector<std::uint32_t>>& rows, const std::string& strings) {
    const unsigned count = fields.size(), stride = count * 4, data = 16 + count * 12;
    std::vector<std::uint8_t> bytes(data + rows.size() * stride);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[at + i] = value >> (24 - 8 * i);
    };
    put(0, rows.size()), put(4, count), put(8, data), put(12, stride);
    for (unsigned f = 0; f < count; ++f) {
        put(16 + f * 12, JGadget::getHashCode(fields[f].first)), put(20 + f * 12, 0xffffffff);
        bytes[24 + f * 12] = (f * 4) >> 8, bytes[25 + f * 12] = f * 4, bytes[27 + f * 12] = fields[f].second;
    }
    for (std::size_t r = 0; r < rows.size(); ++r)
        for (unsigned f = 0; f < count; ++f) put(data + r * stride + f * 4, rows[r][f]);
    bytes.insert(bytes.end(), strings.begin(), strings.end());
    return bytes;
}

std::uint32_t bits(float value) { std::uint32_t out; std::memcpy(&out, &value, 4); return out; }

std::vector<std::uint8_t> railTable() {
    // Strings: "rail" at 0, "Linear" at 5, "OPEN" at 12.
    return bigEndianTable({{"name", 6}, {"type", 6}, {"closed", 6}, {"num_pnt", 0}, {"l_id", 0}}, {{0, 5, 12, 3, 4}},
                          std::string("rail\0Linear\0OPEN\0", 17));
}

std::vector<std::uint8_t> railPoints() {
    const float corners[3][3]{{0, 0, 0}, {1000, 0, 0}, {1000, 0, 1000}};
    std::vector<std::vector<std::uint32_t>> rows;
    for (const float* p : corners) {
        std::vector<std::uint32_t> row;
        for (int handle = 0; handle < 3; ++handle) for (int axis = 0; axis < 3; ++axis) row.push_back(bits(p[axis]));
        rows.push_back(row);
    }
    return bigEndianTable({{"pnt0_x", 2}, {"pnt0_y", 2}, {"pnt0_z", 2}, {"pnt1_x", 2}, {"pnt1_y", 2}, {"pnt1_z", 2},
                           {"pnt2_x", 2}, {"pnt2_y", 2}, {"pnt2_z", 2}}, rows, "");
}

TVec3f pullAt(const PlanetGravity* gravity, float x, float y, float z) {
    TVec3f pull(0, 0, 0);
    gravity->calcGravity(&pull, TVec3f(x, y, z));
    return pull;
}

// Rows through the original GravityCreator, as GlobalGravityObj places them, in two zones.
void creators(smg::NativeScene& scene) {
    TPos3f identity;
    identity.identity();
    TPos3f turned;
    MR::makeMtxTR(turned.toMtxPtr(), TVec3f(10000, 0, 0), TVec3f(0, 0, 90));
    const int galaxy = scene.addZone("TestGalaxy", identity), zone = scene.addZone("TurnedZone", turned);

    std::vector<Row> shapes(8), zoned(6);
    shapes[0].name = "GlobalCubeGravity";
    shapes[1].name = "GlobalCubeGravity";
    shapes[1].args[0] = 0, shapes[1].args[1] = 1, shapes[1].args[2] = 0;
    shapes[1].switchA = 7;
    shapes[2].name = "GlobalConeGravity";
    shapes[2].scale[0] = shapes[2].scale[1] = shapes[2].scale[2] = 2;
    shapes[2].args[0] = 1, shapes[2].args[1] = 0;
    shapes[3] = shapes[2];
    shapes[3].args[0] = 0;
    shapes[4].name = "GlobalDiskTorusGravity";
    shapes[4].scale[0] = shapes[4].scale[1] = shapes[4].scale[2] = 2;
    shapes[4].args[0] = 0, shapes[4].args[2] = 400;
    shapes[5] = shapes[4];
    shapes[5].args[1] = 1;
    shapes[6].name = "GlobalWireGravity";
    shapes[6].rail = 4, shapes[6].args[0] = 3;
    shapes[7].name = "GlobalPlaneGravityInBox";
    shapes[7].type = "Shadow", shapes[7].power = "Light", shapes[7].inverse = 1, shapes[7].priority = 3, shapes[7].range = 800;
    zoned[0].name = "GlobalPointGravity";
    zoned[0].pos[1] = 1000, zoned[0].priority = 5, zoned[0].range = 3000;
    zoned[1].name = "GlobalPlaneGravity";
    zoned[2].name = "GlobalPointGravity";
    zoned[2].scale[0] = 3, zoned[2].distant = -1;
    // Switches in the second zone: B gates one off, SW_SLEEP puts one to sleep, and A=4 needs switch 4.
    for (int i = 3; i < 6; ++i) zoned[i].name = "GlobalPointGravity", zoned[i].pos[1] = 1000, zoned[i].range = 3000;
    zoned[3].switchB = 3;
    zoned[4].sleep = 4;
    zoned[5].switchA = 4;

    const auto shapeBytes = planetTable(shapes), zonedBytes = planetTable(zoned);
    smg::Bcsv shapeTable, zonedTable;
    std::string error;
    check(shapeTable.load({shapeBytes.data(), shapeBytes.size()}, error) &&
          zonedTable.load({zonedBytes.data(), zonedBytes.size()}, error), "the planetobjinfo tables load");
    scene.addZoneTable(galaxy, shapeTable.info());
    scene.addZoneTable(zone, zonedTable.info());
    const auto railBytes = railTable(), pointBytes = railPoints();
    smg::Bcsv rails, points;
    check(rails.load({railBytes.data(), railBytes.size()}, error) && points.load({pointBytes.data(), pointBytes.size()}, error),
          "the galaxy's path tables load");
    scene.addZoneTable(galaxy, rails.info());
    scene.addZoneTable(galaxy, points.info());
    scene.addZonePaths(galaxy, rails.info(), {&points.info()});
    for (int row = 0; row < shapeTable.info().getNumEntries(); ++row) scene.placeGlobalGravity(JMapInfoIter(&shapeTable.info(), row));
    for (int row = 0; row < zonedTable.info().getNumEntries(); ++row) scene.placeGlobalGravity(JMapInfoIter(&zonedTable.info(), row));
    scene.syncAfterPlacement();
    // Levers: what a switch object does with its own row's switches, the original StageSwitchCtrl.
    StageSwitchCtrl* leverA7 = MR::createStageSwitchCtrl(nullptr, JMapInfoIter(&shapeTable.info(), 1));
    StageSwitchCtrl* leverB3 = MR::createStageSwitchCtrl(nullptr, JMapInfoIter(&zonedTable.info(), 3));
    StageSwitchCtrl* leverA4 = MR::createStageSwitchCtrl(nullptr, JMapInfoIter(&zonedTable.info(), 5));
    scene.forgetZoneTables();

    const auto& placed = scene.placedGravities();
    check(placed.size() == 14 && scene.skippedGravities().empty(), "every kind is placed, wire gravity too");
    check(placed[0].zone == galaxy && placed[8].zone == zone, "each gravity knows its zone");
    for (const auto& gravity : placed) check(gravity.gravity->mIsRegistered, "each gravity is registered with the manager");

    // A cube stands on its placement point, 500 units out from its center per unit of scale.
    const PlanetGravity* cube = placed[0].gravity;
    check(vec(pullAt(cube, 0, 3000, 0), 0, -1, 0), "above the cube, down onto its top face");
    check(vec(pullAt(cube, 3000, 500, 0), -0.64f, 0, 0), "beside it, onto the +X face, weaker farther out");
    const float edge = 0.5f * std::sqrt(0.5f), corner = std::sqrt(1.0f / 3.0f) / 3.0f;
    check(vec(pullAt(cube, 2500, 3000, 0), -edge, -edge, 0), "past an edge, toward the edge");
    check(vec(pullAt(cube, 2500, 3000, 2500), -corner, -corner, -corner), "past a corner, toward the corner");
    const PlanetGravity* topOnly = placed[1].gravity;
    check(vec(pullAt(topOnly, 0, 3000, 0), 0, -1, 0) && vec(pullAt(topOnly, 3000, 500, 0), 0, 0, 0), "a cube with only its top face");
    check(!topOnly->mActivated, "switch A gates a gravity off while the scene starts");
    check(scene.startStateGravities().empty(), "switches are followed, so no row keeps only its start state");
    leverA7->onSwitchA();
    scene.movement();
    check(topOnly->mActivated && vec(pullAt(topOnly, 0, 3000, 0), 0, -1, 0), "turning switch 7 on turns the cube on");
    leverA7->offSwitchA();
    scene.movement();
    check(!topOnly->mActivated, "and off again");

    const PlanetGravity* gatedB = placed[11].gravity;
    const PlanetGravity* sleeper = placed[12].gravity;
    const PlanetGravity* needs4 = placed[13].gravity;
    check(gatedB->mActivated && gatedB->mAppeared, "a gravity with only switch B starts on");
    leverB3->onSwitchB();
    scene.movement();
    check(!gatedB->mActivated, "switch B on turns it off");
    leverB3->offSwitchB();
    scene.movement();
    check(gatedB->mActivated, "and switch B off turns it back on");
    check(!topOnly->mActivated, "another zone's switch leaves the cube alone");

    check(!sleeper->mAppeared, "SW_SLEEP off puts a gravity to sleep when the scene starts, as initSync does");
    check(!needs4->mActivated, "switch A=4 is off too");
    leverA4->onSwitchA();
    scene.movement();
    check(sleeper->mAppeared && needs4->mActivated, "switch 4 on wakes the sleeper and turns the A=4 gravity on");
    leverA4->offSwitchA();
    scene.movement();
    check(!sleeper->mAppeared && !needs4->mActivated, "and off puts them back");
    for (StageSwitchCtrl* lever : {leverA7, leverB3, leverA4}) {
        for (SwitchIdInfo* info : {lever->mSW_A, lever->mSW_B, lever->mSW_Appear, lever->mSW_Dead}) {
            if (info) delete info->mIDInfo;
            delete info;
        }
        delete lever;
    }

    // Cone of scale 2: base radius 1000 at the placement point, apex 1000 above it.
    const PlanetGravity* cone = placed[2].gravity;
    const float side = std::sqrt(0.5f);
    check(vec(pullAt(cone, 0, 3000, 0), 0, -1, 0), "above the apex, straight down the axis");
    check(vec(pullAt(cone, 500 + 2000 * side, 500 + 2000 * side, 0), -side, -side, 0), "out from the side, back onto it");
    check(vec(pullAt(cone, 300, -2000, 0), 0, 1, 0), "under an enabled base, up onto it");
    check(vec(pullAt(placed[3].gravity, 300, -2000, 0), 0, 0, 0), "a cone without its base has no pull there");

    // A flat ring from radius 600 to 1000 (scale 2, band 400), felt from above only.
    const PlanetGravity* ring = placed[4].gravity;
    check(vec(pullAt(ring, 800, 2000, 0), 0, -1, 0), "over the ring, straight down");
    check(vec(pullAt(ring, 800, -2000, 0), 0, 0, 0), "a one-sided ring has no pull beneath");
    check(vec(pullAt(ring, 200, 0, 0), 25, 0, 0), "in the hole, out to the inner edge");
    check(vec(pullAt(ring, 3000, 0, 0), -1, 0, 0), "outside, in to the outer edge");
    check(vec(pullAt(placed[5].gravity, 3000, 0, 0), 0, 0, 0), "an outer edge turned off has no pull outside");

    // Rail 4 is 2000 long; Obj_arg0 3 asks for five samples 500 apart, and the creator's loop takes four.
    // Each check stands 400 from the wire, where this row's power gives a pull of 25.
    const PlanetGravity* wire = placed[6].gravity;
    check(vec(pullAt(wire, 500, 400, 0), 0, -25, 0), "a wire gravity pulls onto its rail's first span");
    check(vec(pullAt(wire, 1400, 0, 250), -25, 0, 0), "and its second");
    check(vec(pullAt(wire, 1000, 0, 900), 0, 0, -25), "its last sample is 1500 along; the rail's end is not in the wire");

    const PlanetGravity* box = placed[7].gravity;
    check(box->mGravityType == GRAVITY_TYPE_SHADOW && box->mGravityPower == GRAVITY_POWER_LIGHT && box->mIsInverse &&
          box->mPriority == 3 && box->mRange == 800, "the row's own gravity parameters");

    // The second zone is moved 10000 along X and turned 90 degrees about Z.
    check(vec(pullAt(placed[8].gravity, 9000, 0, 2000), 0, 0, -1), "a point gravity moves with its zone");
    check(vec(pullAt(placed[9].gravity, 50000, 7, -3), 1, 0, 0), "a plane gravity turns with its zone");
    check(placed[10].gravity->mDistant == 1500, "a negative Distant keeps the size from the scale");
}

// LightData.bcsv with a first row (what an unknown name finds) and a named one, as the disc stores them.
std::vector<std::uint8_t> lightData() {
    std::vector<std::pair<const char*, unsigned>> fields{{"AreaLightName", 6}, {"Interpolate", 0}, {"Fix", 0}};
    static const char* const names[]{"PlanetLight0ColorR", "PlanetLight0ColorG", "PlanetLight0ColorB", "PlanetLight0ColorA",
        "PlanetLight0PosX", "PlanetLight0PosY", "PlanetLight0PosZ", "PlanetLight0FollowCamera", "PlanetLight1ColorR",
        "PlanetLight1ColorG", "PlanetLight1ColorB", "PlanetLight1ColorA", "PlanetLight1PosX", "PlanetLight1PosY",
        "PlanetLight1PosZ", "PlanetLight1FollowCamera", "PlanetAlpha2", "PlanetAmbientR", "PlanetAmbientG",
        "PlanetAmbientB", "PlanetAmbientA"};
    for (const char* name : names) fields.push_back({name, std::strstr(name, "Pos") ? 2u : 0u});
    // Colors are stored as signed bytes: -1 is 255 and -128 is 128.
    const auto row = [](std::uint32_t name, float x, std::int32_t red) {
        return std::vector<std::uint32_t>{name, 30, 1, static_cast<std::uint32_t>(red), 0xffffffffu, 0xffffffffu, 0, bits(x), bits(0),
                                          bits(0), 0, 100, 0xffffff96u, 0xffffffffu, 0, bits(0), bits(-1), bits(0), 1, 0xffffff80u, 64,
                                          64, 64, 0xffffff80u};
    };
    return bigEndianTable(fields, {row(0, 1000, -1), row(6, 2000, 100), row(10, 3000, 50)}, std::string("First\0Egg\0Night\0", 16));
}

std::vector<std::uint8_t> galaxyLights() {
    return bigEndianTable({{"LightID", 0}, {"AreaLightName", 6}}, {{0xffffffffu, 0}, {3, 4}}, std::string("Egg\0Night\0", 10));
}

void lights() {
    TPos3f identity;
    identity.identity();
    const auto dataBytes = lightData(), zoneBytes = galaxyLights();
    for (const bool withTable : {true, false}) {
        smg::NativeScene scene;
        scene.addZone("TestGalaxy", identity);
        scene.addZone("PlainZone", identity);
        auto data = std::make_unique<smg::Bcsv>();
        std::string error;
        check(data->load({dataBytes.data(), dataBytes.size()}, error), "LightData.bcsv loads");
        std::vector<std::pair<std::string, std::unique_ptr<smg::Bcsv>>> zones;
        if (withTable) {
            zones.emplace_back("lighttestgalaxy.bcsv", std::make_unique<smg::Bcsv>());
            check(zones.back().second->load({zoneBytes.data(), zoneBytes.size()}, error), "the galaxy's light table loads");
        }
        scene.initLights(std::move(data), std::move(zones));
        const AreaLightInfo* light = scene.defaultAreaLight();
        if (!withTable) {
            check(light && std::strcmp(light->mAreaLightName, "First") == 0, "without a table the first light stands in");
            continue;
        }
        check(light && std::strcmp(light->mAreaLightName, "Egg") == 0, "LightID -1 in the galaxy's table names its default light");
        const ActorLightInfo& planet = light->mPlanetLight;
        check(planet.mInfo0.mColor.r == 100 && planet.mInfo0.mColor.g == 255 && planet.mInfo0.mColor.a == 0 &&
              planet.mInfo0.mPos.x == 2000 && !planet.mInfo0.mIsFollowCamera, "the planet light's first light, bytes and all");
        check(planet.mInfo1.mColor.g == 150 && planet.mInfo1.mPos.y == -1 && planet.mInfo1.mIsFollowCamera,
              "its second light follows the camera");
        check(planet.mAlpha2 == 128 && planet.mColor.r == 64 && planet.mColor.a == 128, "alpha light and ambient");
    }
}

// Light areas as NameObjFactory makes them from areaobjinfo rows, through AreaObj::init.
void lightAreas() {
    TPos3f identity;
    identity.identity();
    smg::NativeScene scene;
    const int galaxy = scene.addZone("TestGalaxy", identity);
    const auto real = [](float v) { return bits(v); };
    // Strings: "LightCtrlCube" at 0, "WaterCube" at 14.
    const std::uint32_t none = 0xffffffffu;
    const auto areaBytes = bigEndianTable(
        {{"name", 6}, {"pos_x", 2}, {"pos_y", 2}, {"pos_z", 2}, {"dir_x", 2}, {"dir_y", 2}, {"dir_z", 2}, {"scale_x", 2},
         {"scale_y", 2}, {"scale_z", 2}, {"Obj_arg0", 0}, {"Obj_arg1", 0}, {"SW_APPEAR", 0}, {"SW_A", 0}},
        {{0, real(0), real(0), real(0), real(0), real(0), real(0), real(1), real(1), real(1), 3, 0, none, none},
         {0, real(5000), real(0), real(0), real(0), real(0), real(0), real(1), real(1), real(1), 3, 0, 9, none},
         {14, real(0), real(0), real(0), real(0), real(0), real(0), real(1), real(1), real(1), none, none, none, 9}},
        std::string("LightCtrlCube\0WaterCube\0", 24));
    smg::Bcsv areas;
    std::string error;
    check(areas.load({areaBytes.data(), areaBytes.size()}, error), "the areaobjinfo table loads");
    scene.addZoneTable(galaxy, areas.info());
    for (int row = 0; row < areas.info().getNumEntries(); ++row) scene.placeAreaObj(JMapInfoIter(&areas.info(), row));
    StageSwitchCtrl* lever = MR::createStageSwitchCtrl(nullptr, JMapInfoIter(&areas.info(), 2));
    scene.syncAfterPlacement();
    scene.forgetZoneTables();
    check(scene.areaCount() == 2 && scene.skippedAreas().size() == 1 && scene.skippedAreas()[0] == "WaterCube",
          "light areas are placed; other areas wait, and say so");

    const auto dataBytes = lightData(), zoneBytes = galaxyLights();
    auto data = std::make_unique<smg::Bcsv>();
    std::vector<std::pair<std::string, std::unique_ptr<smg::Bcsv>>> zones;
    zones.emplace_back("lighttestgalaxy.bcsv", std::make_unique<smg::Bcsv>());
    check(data->load({dataBytes.data(), dataBytes.size()}, error) && zones.back().second->load({zoneBytes.data(), zoneBytes.size()}, error),
          "the light tables load");
    scene.initLights(std::move(data), std::move(zones));
    const auto lightAt = [&](float x, float y, float z) { return std::string(scene.areaLightAt(TVec3f(x, y, z))->mAreaLightName); };
    check(lightAt(0, 500, 0) == "Night", "inside a light area, the light its zone table names for Obj_arg0");
    check(lightAt(0, -100, 0) == "Egg" && lightAt(700, 500, 0) == "Egg", "outside it, the galaxy's default");
    check(lightAt(5000, 500, 0) == "Egg", "an area with SW_APPEAR waits for its switch");
    lever->onSwitchA();
    scene.movement();
    check(lightAt(5000, 500, 0) == "Night", "and lights when the switch turns on");
    lever->offSwitchA();
    scene.movement();
    check(lightAt(5000, 500, 0) == "Egg", "and stops when it turns off");
    for (SwitchIdInfo* info : {lever->mSW_A, lever->mSW_B, lever->mSW_Appear, lever->mSW_Dead}) {
        if (info) delete info->mIDInfo;
        delete info;
    }
    delete lever;
}

void sceneObjects() {
    check(!MR::isExistSceneObj(SceneObj_PlanetGravityManager), "no scene objects before a scene");
    smg::NativeScene scene;
    check(MR::isExistSceneObj(SceneObj_PlanetGravityManager), "the scene makes its gravity manager, as GameScene does");
    check(!MR::isExistSceneObj(SceneObj_StageDataHolder), "objects not brought up are absent");
    auto* manager = MR::getSceneObj<PlanetGravityManager>(SceneObj_PlanetGravityManager);
    check(MR::createSceneObj(SceneObj_PlanetGravityManager) == manager, "creating it again returns the same object");
    check(scene.nameObjHolder().find("重力") == manager, "it registered as a NameObj under its original name");
    gravityManager(*manager);
}
} // namespace

int runSceneTests() {
    sceneObjects();
    lights();
    lightAreas();
    {
        // A new scene, so its manager holds only the gravities placed here.
        smg::NativeScene scene;
        creators(scene);
    }
    check(!MR::isExistSceneObj(SceneObj_PlanetGravityManager), "the scene's objects go with it");
    std::printf("%u original scene and gravity checks passed\n", checks);
    return 0;
}

#ifndef SMG_TEST_RUNNER
int main() { return runSceneTests(); }
#endif
