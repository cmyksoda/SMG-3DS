#pragma once
#include <smg/Input.hpp>
#include <citro2d.h>

namespace smg {
class NativePad;
class SceneRenderer;
class TouchUi {
public:
    bool init();
    void shutdown();
    void draw(const InputFrame& input, const Settings& settings, const NativePad& pad, SceneRenderer& scene,
              bool coreReady, const char* assetStatus, bool motionReady);
private:
    void drawTop(const InputFrame& input, const NativePad& pad, SceneRenderer& scene, bool coreReady, const char* assetStatus);
    void drawPlayPage(const InputFrame& input, const Settings& settings, bool motionReady);
    void drawOptionsPage(const InputFrame& input, const Settings& settings);
    void drawRemapPage(const InputFrame& input, const Settings& settings);
    void background(float width, unsigned phase);
    void label(const char* text, float x, float y, float size, u32 color, bool center = false);
    void button(const Rect& bounds, bool active, bool pressed, float alpha, C2D_SpriteSheet icon,
                const char* fallback, const char* caption);
    void chip(const Rect& bounds, const char* text, bool active, bool pressed);
    C3D_RenderTarget* top_ = nullptr;
    C3D_RenderTarget* bottom_ = nullptr;
    C2D_TextBuf text_ = nullptr;
    C2D_SpriteSheet starBit_ = nullptr, camera_ = nullptr, pointer_ = nullptr;
    unsigned frame_ = 0;
};
} // namespace smg
