#pragma once
#include <smg/GxCommands.hpp>
#include <atomic>
#include <cstdint>

namespace smg {
// The 3DS side of the command processor's host (source/compat/GxRender.cpp): a primitive drawn with citro3d, and an
// EFB copy (to the display, ending the frame).
void gxRenderPrimitive(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices);
void gxRenderCopy(const GxCommandProcessor& gpu, std::uint32_t command);
// GX_CallDisplayList, before the command processor reads it: true when the renderer drew the list itself (GPU path).
bool gxRenderDisplayList(const GxCommandProcessor& gpu, std::uint32_t address, std::uint32_t size);
// The CPU changed memory the GPU reads (the game's cache flushes): cached meshes made from it are dropped.
void gxRenderMemoryChanged(const void* start, std::uint32_t size);
// While on, texture copies are kept in the PICA's memory and never written to the Wii's: for copies only the GPU reads
// (JUTTexture::capture's). Every other copy is encoded into memory by the CPU, as the pixel engine writes it. The game's
// side writes the switch into the GX stream (the port's command 0x01); the renderer's takes it there, in order.
void gxRenderCopiesStayOnGpu(bool stay);
void gxRenderKeepCopiesOnGpu(bool stay);
// The GX stream's reader (source/compat/GxFifo.cpp): ticks spent reading and drawing it, ticks the game waited for the
// render thread (for ring room, for the frame before to be drawn, for everything to be drawn before a peek or movie
// frame), bytes read, and the core the render thread runs on (-1: none, the game's own thread draws).
struct GxThreadClock {
    std::atomic<std::uint32_t> busy{0}, gameWaits{0}, frameWaits{0}, drainWaits{0}, bytes{0};
    int core = -1;
};
GxThreadClock& gxThreadClock();
// A THP movie frame's planes (smg::decodeThpVideo), drawn over the EFB rectangle (x, top, w, h); `generation` changes per decode.
void gxRenderMovieFrame(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, unsigned width, unsigned height, unsigned generation,
                        int x, int top, int w, int h);
// GXPeekARGB (0xAARRGGBB) or GXPeekZ (24 bits, nearer smaller) of one EFB pixel, after everything drawn so far.
std::uint32_t gxRenderPeek(const GxCommandProcessor& gpu, unsigned x, unsigned y, bool depth);
} // namespace smg
