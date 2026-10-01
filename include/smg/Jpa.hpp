#pragma once
#include <cstddef>
#include <cstdint>

namespace smg {
// A JParticle container (JPAC 2-10, the game's Particles.jpc) turned native in place for JPAResourceLoader and the
// blocks that read their data from it afterwards; returns why it can't be, leaving it as it was, or null. Each block's
// tag turns too, since the loader compares tags as multi-character constants; texels stay the GPU's.
const char* nativeJpc(std::uint8_t* data, std::size_t size);
} // namespace smg
