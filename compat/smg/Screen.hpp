#pragma once
// The top screen is 5:3 and shows the EFB's whole 640x456, so the game's screen space (what 2D draws and the pointer
// share) is square at 760 columns by 456 lines. tools/prepare-petari.py puts these where the game's wide mode says 16:9.

namespace smg::screen {
inline constexpr int Lines = 456;       // the EFB's height, and a layout's and a 2D draw's in every mode
inline constexpr int EfbWidth = 640;    // what a screen-space draw is stretched to fill
inline constexpr int LayoutWidth = 608; // the columns every layout is authored in, whatever the screen's shape
inline constexpr int Width = Lines * 5 / 3;
inline constexpr float Aspect = 5.0f / 3.0f;

// Layout panes that adjust to the screen scale in x by this, so 608 layout columns come out square across Width.
inline constexpr float LayoutAdjustX = static_cast<float>(LayoutWidth) / Width;
// An EFB pixel's width over its height on screen: a blur offset in EFB pixels scales its vertical by this to be round.
inline constexpr float PixelAspect = static_cast<float>(Width) / EfbWidth;
// A picture framed for 16:9 keeps its width on the 5:3 screen and shows 15/16 of its height.
inline constexpr float MovieHeightScale = 15.0f / 16.0f;
// The movies' 368 lines at that scale; the cinema frame's bars (44 lines each at 16:9) take up the rest.
inline constexpr int MovieLines = static_cast<int>(368 * MovieHeightScale);
inline constexpr int MovieBarTop = (Lines - MovieLines) / 2, MovieBarBottom = MovieBarTop + MovieLines;

static_assert(Width * 240 == 400 * Lines, "a screen-space column and line are the same size on the 400x240 screen");
static_assert(Width * 3 == Lines * 5, "the width is the aspect's share of the lines, with nothing rounded");
} // namespace smg::screen
