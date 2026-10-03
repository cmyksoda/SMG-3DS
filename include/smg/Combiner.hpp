#pragma once
#include <smg/Model.hpp>
#include <array>
#include <cstdint>

namespace smg {
// A PICA200 fragment combiner program, independent of citro3d so it can be tested on the host.
enum class CombineSource : std::uint8_t { Primary, Texture0, Texture1, Texture2, Constant, Previous, Buffer };
enum class CombineOperand : std::uint8_t {
    Color, OneMinusColor, Alpha, OneMinusAlpha, Red, OneMinusRed, Green, OneMinusGreen, Blue, OneMinusBlue
};
enum class CombineFunc : std::uint8_t { Replace, Modulate, Add, AddSigned, Interpolate, Subtract, MultiplyAdd };

struct CombineStage {
    CombineSource colorSource[3]{CombineSource::Previous, CombineSource::Previous, CombineSource::Previous};
    CombineSource alphaSource[3]{CombineSource::Previous, CombineSource::Previous, CombineSource::Previous};
    CombineOperand colorOperand[3]{};
    CombineOperand alphaOperand[3]{CombineOperand::Alpha, CombineOperand::Alpha, CombineOperand::Alpha};
    CombineFunc colorFunc = CombineFunc::Replace, alphaFunc = CombineFunc::Replace;
    std::uint8_t colorScale = 1, alphaScale = 1;
    std::array<float, 4> constant{};
};

struct CombinerProgram {
    static constexpr unsigned MaxStages = 6, MaxUnits = 3;
    // A unit's texture when it samples the CPU path's palette of C1 values (compileTev's color1FromUnit).
    static constexpr int Palette = -2;
    unsigned stageCount = 0;
    CombineStage stages[MaxStages];
    // The buffer reads as zero in stage 0 and as bufferColor after; a stage whose bit is set in
    // bufferUpdate (color, alpha) replaces it from two stages later.
    std::array<float, 4> bufferColor{};
    std::uint8_t bufferUpdate[2]{};
    unsigned unitCount = 0;
    struct Unit { int texture = -1; std::uint8_t texCoord = 0; } units[MaxUnits];
    bool exact = true; // false when some GX feature had to be approximated
    const char* approximation = nullptr; // the first reason, for audits
};

// color1FromUnit: C1 is read from a unit of its own (a texel per primitive) instead of as a constant.
CombinerProgram compileTev(const ModelMaterial& material, bool color1FromUnit = false);
} // namespace smg
