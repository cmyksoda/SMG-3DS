#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace smg {
using Transform = std::array<float, 12>; // row-major 3x4

struct StageObject {
    std::string name, zone;
    Transform transform{};
    std::int32_t args[8]{-1, -1, -1, -1, -1, -1, -1, -1}; // Obj_arg0..7, -1 when unset
};

struct StageZone {
    std::string name;
    Transform placement{};
    std::array<float, 3> position{}, rotation{}; // the galaxy's stageobjinfo row; degrees
    std::int32_t layerMask = 0;
};

struct Stage {
    std::string galaxy;
    int scenario = 1;
    std::vector<StageObject> objects;
    std::vector<StageZone> zones; // the galaxy itself first, then the zones it places
    std::array<float, 3> start{}, startRotation{};
    bool entrance = false; // MarioNo 0 is in an active layer; hidden-star scenarios have none
};

// "common" plus one name per bit of a scenario's layer mask: "layera" for bit 0, and so on.
std::vector<std::string> layerNames(std::int32_t mask);

// T * Rz * Ry * Rx * S with angles in degrees, as MR::makeMtxTRS builds placements.
Transform placementMatrix(const float position[3], const float rotation[3], const float scale[3]);
Transform multiply(const Transform& a, const Transform& b);

// Collects every object and map part a scenario's layers place, in world space, from the
// galaxy archive and each zone archive under root/StageData.
bool loadStage(const std::string& root, const std::string& galaxy, int scenario, Stage& stage, std::string& error);
} // namespace smg
