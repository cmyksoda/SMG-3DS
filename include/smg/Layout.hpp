#pragma once
#include <cstddef>
#include <cstdint>

namespace smg {
// nw4r's layout family turned native in place for nw4r's own readers: BRLYT for Layout::Build and the panes,
// materials and groups it builds; BRLAN for AnimResource and AnimTransformBasic; TPL for TPLBind and TexMap (texel
// and palette data stay as the GPU reads them); BRFNT for ResFont. Each returns why a file cannot be turned, leaving
// it as it was, or null once it is native (a second call changes nothing).
const char* nativeBrlyt(std::uint8_t* data, std::size_t size);
const char* nativeBrlan(std::uint8_t* data, std::size_t size);
const char* nativeTpl(std::uint8_t* data, std::size_t size);
const char* nativeBrfnt(std::uint8_t* data, std::size_t size);
} // namespace smg
