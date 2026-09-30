#pragma once
#include <string>

namespace smg {
class NativeScene;

// Reads ObjectData/LightData.arc into the scene's light holders, as LightDirector loads it after placement.
// The scene's zones must be registered first; LightZoneDataHolder reads one table per zone.
bool loadStageLights(const std::string& root, NativeScene& scene, std::string& error);
} // namespace smg
