#pragma once
#include <smg/GxCommands.hpp>
#include <cstdint>

namespace smg {
// The 3DS side of the command processor's host (source/compat/GxRender.cpp): a primitive drawn with citro3d, and an
// EFB copy (to the display, ending the frame).
void gxRenderPrimitive(const GxCommandProcessor& gpu, std::uint8_t kind, std::uint8_t format, std::uint16_t count, const std::uint8_t* vertices);
void gxRenderCopy(const GxCommandProcessor& gpu, std::uint32_t command);
// GXPeekARGB (0xAARRGGBB) or GXPeekZ (24 bits, nearer smaller) of one EFB pixel, after everything drawn so far.
std::uint32_t gxRenderPeek(const GxCommandProcessor& gpu, unsigned x, unsigned y, bool depth);
} // namespace smg
