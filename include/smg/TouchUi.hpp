#pragma once
#include <smg/Input.hpp>
#include <citro2d.h>

namespace smg {
class TouchUi {
public:
    bool init();
    void shutdown();
    void draw(const InputFrame& input, bool coreReady, const char* assetStatus);
private:
    void background(float width, unsigned phase);
    void label(const char* text, float x, float y, float size, u32 color, bool center = false);
    void button(const Rect& bounds, bool active, bool pressed, float alpha, C2D_SpriteSheet icon, const char* fallback);
    C3D_RenderTarget* top_ = nullptr;
    C3D_RenderTarget* bottom_ = nullptr;
    C2D_TextBuf text_ = nullptr;
    C2D_SpriteSheet starBit_ = nullptr, camera_ = nullptr, pointer_ = nullptr;
    unsigned frame_ = 0;
};
} // namespace smg
