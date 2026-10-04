// The Wii's THP movies on the 3DS. Frames are decoded by smg::decodeThpVideo into plain 8-bit planes, in the buffers the
// game hands THPVideoDecode; only THPGXYuv2RgbDraw reads them, through the renderer, which turns each decoded frame into
// one texture. The Wii turned YUV to RGB in TEV stages whose signed sums the PICA's clamped combiner can't hold.
#include <revolution/thp.h>
#include <smg/FreeStore.hpp>
#include <smg/Thp.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" void smgDrawMovieFrame(const u8* y, const u8* u, const u8* v, unsigned width, unsigned height, unsigned generation, int x,
                                  int top, int w, int h);

namespace {
// The movie's size, the frame waiting to be decoded into them and the decode count, by the luma plane they go into (a
// player keeps two texture sets).
struct Planes {
    u8 *y = nullptr, *u = nullptr, *v = nullptr;
    unsigned width = 0, height = 0, generation = 0;
    std::vector<u8, smg::FreeStoreAllocator<u8>> pending;
};
std::array<Planes, 4> sPlanes;
unsigned sDecodes = 0;

Planes* planesFor(const void* y) {
    for (Planes& p : sPlanes)
        if (p.y == y) return &p;
    for (Planes& p : sPlanes)
        if (!p.y) return &p;
    return &sPlanes[sDecodes % sPlanes.size()];
}

[[noreturn]] void stop(const char* what) {
    std::fprintf(stderr, "The Wii's THP movies: %s is not the port's yet\n", what);
    std::abort();
}
} // namespace

extern "C" {
// The locked cache (16 KB of the Wii CPU's data cache as a scratchpad) staged the SDK's decoding; the port's needs none.
void LCEnable() {}
void LCDisable() {}

BOOL THPInit() { return TRUE; }

// Movie frames decoded so far, for the renderer's perf line (how the player paces itself when the game runs slowly).
unsigned smgMovieDecodes() { return sDecodes; }

// The video component's size, from THPSimplePlayerWrapper::decode (adapted) just before each THPVideoDecode.
u32 smgThpFrameBytes = 0;

// A frame's video component, at most a read buffer long. It's decoded when drawn: the player decodes each logic step
// into the texture set it draws next, the port draws every second step, and the read buffer is refilled meanwhile.
s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void*) {
    if (!file || !tileY || !tileU || !tileV) return 1;
    const smg::Bytes frame{static_cast<const u8*>(file), std::min<u32>(smgThpFrameBytes ? smgThpFrameBytes : 0x20000, 0x20000)};
    unsigned width = 0, height = 0;
    if (const char* why = smg::thpVideoSize(frame, width, height)) stop(why);
    Planes& planes = *planesFor(tileY);
    planes.y = static_cast<u8*>(tileY), planes.u = static_cast<u8*>(tileU), planes.v = static_cast<u8*>(tileV);
    planes.width = width, planes.height = height;
    planes.pending.assign(frame.data, frame.data + frame.size);
    return 0;
}

void THPGXYuv2RgbDraw(u8* y, u8* u, u8* v, s16 x, s16 top, s16 textureWidth, s16 textureHeight, s16 polygonWidth, s16 polygonHeight) {
    Planes& planes = *planesFor(y);
    if (planes.y != y || planes.u != u || planes.v != v || planes.width != unsigned(textureWidth) || planes.height != unsigned(textureHeight))
        stop("drawing a movie frame that was never decoded");
    if (!planes.pending.empty()) {
        if (const char* why = smg::decodeThpVideo({planes.pending.data(), planes.pending.size()}, planes.width, planes.height, y, u, v)) stop(why);
        planes.pending.clear(), planes.generation = ++sDecodes;
    }
    smgDrawMovieFrame(y, u, v, planes.width, planes.height, planes.generation, x, top, polygonWidth, polygonHeight);
}
}
