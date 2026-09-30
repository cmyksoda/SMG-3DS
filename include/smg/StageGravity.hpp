#pragma once
#include <smg/Stage.hpp>
#include <string>

namespace smg {
class NativeScene;

// Places every global gravity in the scenario's active planetobjinfo tables, zone by zone, through the
// original GravityCreator, and the light areas of its areaobjinfo tables, as the game does while it reads a stage.
bool placeStageGravity(const std::string& root, const Stage& stage, NativeScene& scene, std::string& error);
} // namespace smg
