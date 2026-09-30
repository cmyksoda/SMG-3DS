#pragma once
#include <array>
#include <string>
#include <vector>

namespace smg {
using Transform = std::array<float, 12>; // row-major 3x4

struct StageObject {
    std::string name, zone;
    Transform transform{};
};

struct Stage {
    std::string galaxy;
    int scenario = 1;
    std::vector<StageObject> objects;
    std::array<float, 3> start{}, startRotation{};
};

// T * Rz * Ry * Rx * S with angles in degrees, as MR::makeMtxTRS builds placements.
Transform placementMatrix(const float position[3], const float rotation[3], const float scale[3]);
Transform multiply(const Transform& a, const Transform& b);

// Collects every object and map part a scenario's layers place, in world space, from the
// galaxy archive and each zone archive under root/StageData.
bool loadStage(const std::string& root, const std::string& galaxy, int scenario, Stage& stage, std::string& error);
} // namespace smg
