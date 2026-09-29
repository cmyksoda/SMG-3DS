#include <smg/TouchUi.hpp>
#include <smg/NativePad.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace smg {
namespace {
u32 color(int r, int g, int b, float a = 1.0f) {
    return C2D_Color32(r, g, b, static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f));
}
void image(C2D_SpriteSheet sheet, float x, float y, float size, float alpha) {
    const C2D_Image img = C2D_SpriteSheetGetImage(sheet, 0);
    const float scale = size / std::max(img.subtex->width, img.subtex->height);
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawImageAt(img, x - img.subtex->width*scale/2, y - img.subtex->height*scale/2, 0, &tint, scale, scale);
}
void starPointer(C2D_SpriteSheet sheet, float x, float y, bool shooting) {
    const C2D_Image left = C2D_SpriteSheetGetImage(sheet, 0);
    // DPDPointer.brlyt: PicPointer samples DPDPointer.tpl with scale S=2, mirror S.
    Tex3DS_SubTexture reflected = *left.subtex;
    std::swap(reflected.left, reflected.right);
    C2D_Image right = left;
    right.subtex = &reflected;
    const float scale = 28.0f / left.subtex->height;
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, shooting ? color(255, 190, 80) : color(10, 110, 250), 0.55f);
    C2D_DrawImageAt(left, x-left.subtex->width*scale, y-14, 0, &tint, scale, scale);
    C2D_DrawImageAt(right, x, y-14, 0, &tint, scale, scale);
}
C2D_SpriteSheet loadIcon(const char* path) {
    C2D_SpriteSheet sheet = C2D_SpriteSheetLoad(path);
    if (sheet && C2D_SpriteSheetCount(sheet) == 0) {
        C2D_SpriteSheetFree(sheet);
        return nullptr;
    }
    return sheet;
}
}

bool TouchUi::init() {
    top_ = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottom_ = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    text_ = C2D_TextBufNew(2048);
    starBit_ = loadIcon("sdmc:/3ds/SMG-3DS/ui/star-bit.t3x");
    camera_ = loadIcon("sdmc:/3ds/SMG-3DS/ui/camera.t3x");
    pointer_ = loadIcon("sdmc:/3ds/SMG-3DS/ui/pointer.t3x");
    return top_ && bottom_ && text_;
}
void TouchUi::shutdown() {
    if (starBit_) C2D_SpriteSheetFree(starBit_);
    if (camera_) C2D_SpriteSheetFree(camera_);
    if (pointer_) C2D_SpriteSheetFree(pointer_);
    if (text_) C2D_TextBufDelete(text_);
    if (top_) C3D_RenderTargetDelete(top_);
    if (bottom_) C3D_RenderTargetDelete(bottom_);
}

void TouchUi::label(const char* value, float x, float y, float size, u32 tint, bool center) {
    C2D_Text text;
    C2D_TextParse(&text, text_, value);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor | (center ? C2D_AlignCenter : C2D_AlignLeft), x, y, 0, size, size, tint);
}

void TouchUi::background(float width, unsigned phase) {
    C2D_DrawRectangle(0, 0, 0, width, 240, color(7, 12, 38), color(17, 21, 63), color(25, 22, 65), color(5, 23, 48));
    for (int i = 0; i < 9; ++i) {
        const float radius = 106.0f - i * 9.0f;
        C2D_DrawEllipseSolid(width*0.68f - radius, 94 - radius*0.58f, 0,
            radius*2, radius*1.16f, color(103, 62, 153, 0.028f));
    }
    // Fixed star positions avoid allocation and frame-to-frame flicker.
    for (unsigned i = 0; i < 72; ++i) {
        const float x = static_cast<float>((i * 137 + 29) % static_cast<unsigned>(width));
        const float y = static_cast<float>((i * 89 + 17) % 240);
        const float shimmer = 0.35f + 0.3f * (1.0f + std::sin(phase*0.015f + i));
        const float size = i % 9 == 0 ? 2.0f : 1.0f;
        C2D_DrawRectSolid(x, y, 0, size, size, color(198, 222, 255, shimmer));
    }
}

void TouchUi::button(const Rect& b, bool active, bool pressed, float alpha, C2D_SpriteSheet icon, const char* fallback) {
    const auto edge = active ? color(255, 207, 101, alpha) : color(127, 186, 237, alpha*0.9f);
    const auto fill = active ? color(127, 74, 37, alpha*0.95f) : color(25, 43, 79, alpha*0.94f);
    C2D_DrawRectSolid(b.x+2, b.y+3, 0, b.width, b.height, color(0, 2, 12, alpha*0.4f));
    C2D_DrawRectSolid(b.x, b.y, 0, b.width, b.height, edge);
    C2D_DrawRectSolid(b.x+2, b.y+2, 0, b.width-4, b.height-4, fill);
    if (pressed) C2D_DrawRectSolid(b.x+3, b.y+3, 0, b.width-6, b.height-6, color(255, 255, 255, alpha*0.15f));
    if (icon) image(icon, b.x + b.width/2.0f, b.y + 17.0f + (pressed ? 1 : 0), 25, alpha);
    else label(fallback, b.x + b.width/2.0f, b.y+8.0f, 0.38f, color(229, 238, 255, alpha), true);
    label(active ? "ON" : (icon == starBit_ && fallback[0] == 'B' ? "OFF" : "RESET"),
        b.x + b.width/2.0f, b.y+30.0f, 0.30f, edge, true);
}

void TouchUi::draw(const InputFrame& input, const NativePad& pad, bool coreReady, const char* assetStatus) {
    C2D_TextBufClear(text_);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top_, color(4, 9, 26));
    C2D_SceneBegin(top_);
    background(400, frame_);
    label("SMG-3DS", 200, 18, 0.9f, color(236, 243, 255), true);
    label("native platform bring-up", 200, 49, 0.45f, color(156, 185, 229), true);
    C2D_DrawRectSolid(28, 80, 0, 344, 112, color(8, 17, 42, 0.88f));
    label(coreReady ? "Petari state machine running" : "Starting Petari core...", 42, 91, 0.5f, color(151, 223, 205));
    label("Gameplay engine integration pending", 42, 113, 0.46f, color(226, 230, 244));
    char status[128];
    const auto& buttons = pad.buttons();
    std::snprintf(status, sizeof(status), "WPad %+.2f  %+.2f   %s %s %s", pad.stick().mStick.x, pad.stick().mStick.y,
        buttons.testButtonA() ? "JUMP" : "", pad.sample().swing ? "SPIN" : "", buttons.testButtonZ() ? "CROUCH" : "");
    label(status, 42, 139, 0.37f, color(192, 201, 225));
    label(buttons.testButtonC() ? "Camera recenter input" : (buttons.testButtonB() ? "Star-bit fire input" : "Touch to test the star pointer"),
        42, 161, 0.42f, color(230, 203, 133));
    label(assetStatus, 200, 193, 0.34f, color(173, 183, 206), true);
    label("START + SELECT: exit", 200, 216, 0.36f, color(148, 166, 195), true);
    if (input.pointer.visible) {
        if (pointer_) starPointer(pointer_, input.pointer.x, input.pointer.y, input.shootMode);
        else {
            // A diagnostic reticle until the user's original pointer texture is imported.
            const auto c = input.shootMode ? color(255, 204, 112) : color(138, 221, 255);
            C2D_DrawLine(input.pointer.x-8, input.pointer.y, c, input.pointer.x+8, input.pointer.y, c, 2, 0);
            C2D_DrawLine(input.pointer.x, input.pointer.y-8, c, input.pointer.x, input.pointer.y+8, c, 2, 0);
        }
    }

    C2D_TargetClear(bottom_, color(4, 9, 26));
    C2D_SceneBegin(bottom_);
    background(320, frame_);
    label(input.shootMode ? "STAR BITS  /  SHOOT" : "STAR POINTER  /  COLLECT", 160, 12, 0.43f,
        input.shootMode ? color(255, 218, 147) : color(169, 219, 250), true);
    if (!input.pointer.visible) {
        label("touch the stars", 160, 106, 0.59f, color(209, 222, 246, 0.82f), true);
        if (!starBit_ || !camera_) label("original icons awaiting game data", 160, 133, 0.32f, color(139, 156, 192), true);
    } else {
        const float x = input.pointer.x * 319.0f / 399.0f;
        const float y = input.pointer.y;
        C2D_DrawCircleSolid(x, y, 0, 7, color(195, 221, 255, 0.22f));
        C2D_DrawCircleSolid(x, y, 0, 2, color(222, 238, 255, 0.85f));
    }
    const float opacity = touchButtonOpacity(input);
    button(CameraButton, false, input.cameraButtonHeld, opacity, camera_, "CAM");
    button(ShootButton, input.shootMode, input.shootButtonHeld, opacity, starBit_, "BITS");
    label(input.shootMode ? "shooting enabled" : "collecting star bits", 160, 216, 0.33f, color(168, 191, 221, 0.7f), true);
    C3D_FrameEnd(0);
    ++frame_;
}
} // namespace smg
