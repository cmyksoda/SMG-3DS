#pragma once
#include <smg/Stage.hpp>
#include <string>

namespace smg {
class NativeScene;

// Places every global gravity in the scenario's active planetobjinfo tables, zone by zone, through the
// original GravityCreator, as the game does while it reads a stage.
bool placeStageGravity(const std::string& root, const Stage& stage, NativeScene& scene, std::string& error);
} // namespace smg
