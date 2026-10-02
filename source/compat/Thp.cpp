// The Wii's THP movies on the 3DS. Frames are decoded by smg::decodeThpVideo into plain 8-bit planes, in the buffers the
// game hands THPVideoDecode; only THPGXYuv2RgbDraw reads them, through the renderer, which turns each decoded frame into
// one texture. The Wii turned YUV to RGB in TEV stages whose signed sums the PICA's clamped combiner can't hold.
#include <revolution/thp.h>
#include <smg/Thp.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>

extern "C" void smgDrawMovieFrame(const u8* y, const u8* u, const u8* v, unsigned width, unsigned height, unsigned generation, int x,
                                  int top, int w, int h);

namespace {
// The movie's size and the decode count, by the luma plane they went into (a player keeps two texture sets).
struct Planes {
    const u8* y = nullptr;
    unsigned width = 0, height = 0, generation = 0;
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

// A frame's video component, at most a read buffer long (the bound only matters for a damaged frame).
s32 THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void*) {
    if (!file || !tileY || !tileU || !tileV) return 1;
    const smg::Bytes frame{static_cast<const u8*>(file), 0x20000};
    unsigned width = 0, height = 0;
    if (const char* why = smg::thpVideoSize(frame, width, height)) stop(why);
    if (const char* why = smg::decodeThpVideo(frame, width, height, static_cast<u8*>(tileY), static_cast<u8*>(tileU), static_cast<u8*>(tileV)))
        stop(why);
    Planes& planes = *planesFor(tileY);
    planes = {static_cast<const u8*>(tileY), width, height, ++sDecodes};
    return 0;
}

void THPGXYuv2RgbDraw(u8* y, u8* u, u8* v, s16 x, s16 top, s16 textureWidth, s16 textureHeight, s16 polygonWidth, s16 polygonHeight) {
    const Planes& planes = *planesFor(y);
    if (planes.y != y || planes.width != unsigned(textureWidth) || planes.height != unsigned(textureHeight))
        stop("drawing a movie frame that was never decoded");
    smgDrawMovieFrame(y, u, v, planes.width, planes.height, planes.generation, x, top, polygonWidth, polygonHeight);
}
}
