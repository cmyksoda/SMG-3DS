#pragma once
#include <cstddef>
#include <cstdint>

namespace smg {
// A camera animation ("ANDO" of type CANM or CKAN, read by CameraAnim::loadBin) turned native in place: its header's
// words, each component's count, offset (and key type), the values' byte count and every value. Returns why it can't.
const char* nativeCameraAnimation(std::uint8_t* file, std::size_t size);
} // namespace smg
