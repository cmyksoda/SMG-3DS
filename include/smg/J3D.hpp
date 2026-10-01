#pragma once
#include <cstddef>
#include <cstdint>

namespace smg {
// A J3D model file (BMD, BDL, or a BMT material table) turned native in place for JSystem's own loaders
// (J3DModelLoaderDataBase) and what J3D reads from it afterwards; returns why it can't be turned, leaving it as it
// was, or null. Vertex arrays turn native too, since the CPU reads them and the port's GPU reads arrays in the CPU's
// order; display lists, texels and palettes stay big-endian, as the GPU reads them. `size` bounds the file.
const char* nativeJ3DModel(std::uint8_t* data, std::size_t size);
// A J3D animation file (BCK, BTK, BRK, BPK key animations; BTP, BVA full ones) turned native in place for
// J3DAnmLoaderDataBase and the animation objects that read its tables and values every frame.
const char* nativeJ3DAnimation(std::uint8_t* data, std::size_t size);
// A BTI texture (a ResTIMG header, then the GPU's image and palette) with its header turned native.
const char* nativeBti(std::uint8_t* data, std::size_t size);
} // namespace smg
