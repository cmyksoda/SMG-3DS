#include <smg/TouchUi.hpp>
#include <smg/NativePad.hpp>
#include <smg/SceneRenderer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

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
    C2D_DrawImageAt(img, x - img.subtex->width * scale / 2, y - img.subtex->height * scale / 2, 0, &tint, scale, scale);
}

void starPointer(C2D_SpriteSheet sheet, float x, float y, float height, bool shooting) {
    const C2D_Image left = C2D_SpriteSheetGetImage(sheet, 0);
    // DPDPointer.brlyt: PicPointer samples DPDPointer.tpl with scale S=2, mirror S.
    Tex3DS_SubTexture reflected = *left.subtex;
    std::swap(reflected.left, reflected.right);
    C2D_Image right = left;
    right.subtex = &reflected;
    const float scale = height / left.subtex->height;
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, shooting ? color(255, 190, 80) : color(10, 110, 250), 0.55f);
    C2D_DrawImageAt(left, x - left.subtex->width * scale, y - height / 2, 0, &tint, scale, scale);
    C2D_DrawImageAt(right, x, y - height / 2, 0, &tint, scale, scale);
}

C2D_SpriteSheet loadIcon(const char* path) {
    C2D_SpriteSheet sheet = C2D_SpriteSheetLoad(path);
    if (sheet && C2D_SpriteSheetCount(sheet) == 0) {
        C2D_SpriteSheetFree(sheet);
        return nullptr;
    }
    return sheet;
}

std::string keys(std::uint32_t mask) {
    std::string text;
    for (unsigned bit = 0; bit < KeyCount; ++bit) {
        if (!(mask & (1u << bit))) continue;
        if (!text.empty()) text += " / ";
        text += keyName(bit);
    }
    return text.empty() ? "-" : text;
}

const char* pointerName(PointerMode mode) {
    return mode == PointerMode::Motion ? "MOTION" : mode == PointerMode::CStick ? "C-STICK" : "STYLUS";
}
} // namespace

bool TouchUi::init() {
    top_ = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (top_) {
        C3D_RenderTargetSetOutput(top_, GFX_TOP, GFX_LEFT,
            GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
            GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    }
    bottom_ = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    text_ = C2D_TextBufNew(4096);
    starBit_ = loadIcon("sdmc:/3ds/SMG-3DS/ui/star-bit.t3x");
    camera_ = loadIcon("sdmc:/3ds/SMG-3DS/ui/camera.t3x");
    pointer_ = loadIcon("sdmc:/3ds/SMG-3DS/ui/pointer.t3x");
    // The halves meet at the texture's edge column; sampling past it drew a see-through seam down the star.
    if (pointer_) C3D_TexSetWrap(C2D_SpriteSheetGetImage(pointer_, 0).tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
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
        C2D_DrawEllipseSolid(width * 0.68f - radius, 94 - radius * 0.58f, 0,
            radius * 2, radius * 1.16f, color(103, 62, 153, 0.028f));
    }
    // Fixed star positions avoid allocation and frame-to-frame flicker.
    for (unsigned i = 0; i < 72; ++i) {
        const float x = static_cast<float>((i * 137 + 29) % static_cast<unsigned>(width));
        const float y = static_cast<float>((i * 89 + 17) % 240);
        const float shimmer = 0.35f + 0.3f * (1.0f + std::sin(phase * 0.015f + i));
        const float size = i % 9 == 0 ? 2.0f : 1.0f;
        C2D_DrawRectSolid(x, y, 0, size, size, color(198, 222, 255, shimmer));
    }
}

void TouchUi::button(const Rect& b, bool active, bool pressed, float alpha, C2D_SpriteSheet icon,
                     const char* fallback, const char* caption) {
    const auto edge = active ? color(255, 207, 101, alpha) : color(127, 186, 237, alpha * 0.9f);
    const auto fill = active ? color(127, 74, 37, alpha * 0.95f) : color(25, 43, 79, alpha * 0.94f);
    C2D_DrawRectSolid(b.x + 2, b.y + 3, 0, b.width, b.height, color(0, 2, 12, alpha * 0.4f));
    C2D_DrawRectSolid(b.x, b.y, 0, b.width, b.height, edge);
    C2D_DrawRectSolid(b.x + 2, b.y + 2, 0, b.width - 4, b.height - 4, fill);
    if (pressed) C2D_DrawRectSolid(b.x + 3, b.y + 3, 0, b.width - 6, b.height - 6, color(255, 255, 255, alpha * 0.15f));
    if (icon) image(icon, b.x + b.width / 2.0f, b.y + 17.0f + (pressed ? 1 : 0), 25, alpha);
    else label(fallback, b.x + b.width / 2.0f, b.y + 8.0f, 0.38f, color(229, 238, 255, alpha), true);
    label(caption, b.x + b.width / 2.0f, b.y + 30.0f, 0.30f, edge, true);
}

void TouchUi::chip(const Rect& b, const char* text, bool active, bool pressed) {
    const auto edge = active ? color(255, 207, 101) : color(93, 143, 196);
    const auto fill = active ? color(112, 66, 36) : pressed ? color(45, 70, 116) : color(22, 36, 70);
    C2D_DrawRectSolid(b.x, b.y, 0, b.width, b.height, edge);
    C2D_DrawRectSolid(b.x + 2, b.y + 2, 0, b.width - 4, b.height - 4, fill);
    label(text, b.x + b.width / 2.0f, b.y + b.height / 2.0f - 7, 0.42f,
          active ? color(255, 231, 176) : color(214, 229, 250), true);
}

void TouchUi::drawTop(const InputFrame& input, const NativePad& pad, SceneRenderer& scene, bool coreReady, const char* assetStatus) {
    C3D_RenderTargetClear(top_, C3D_CLEAR_ALL, 0x04091aff, 0);
    if (scene.ready()) {
        C2D_Flush();
        scene.draw(top_);
    }
    C2D_Prepare();
    C2D_SceneBegin(top_);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    if (scene.ready()) {
        C2D_DrawRectSolid(0, 0, 0, 400, 22, color(4, 9, 26, 0.7f));
        label(scene.title().c_str(), 200, 4, 0.4f, color(229, 239, 255), true);
        C2D_DrawRectSolid(0, 206, 0, 400, 34, color(4, 9, 26, 0.7f));
        label(scene.status().c_str(), 200, 208, 0.32f, color(173, 198, 223), true);
        label("Circle Pad fly   C-Stick look   A up   ZL down   X faster   L start view", 200, 223, 0.32f,
              color(194, 210, 230), true);
    } else {
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
        label(buttons.testButtonC() ? "Camera reset input" : buttons.testButtonB() ? "Star-bit fire input" : "Aim the star pointer",
              42, 161, 0.42f, color(230, 203, 133));
        label(assetStatus, 200, 193, 0.34f, color(173, 183, 206), true);
        label("START + SELECT: exit", 200, 216, 0.36f, color(148, 166, 195), true);
        label(scene.status().c_str(), 200, 233, 0.25f, color(173, 183, 206), true);
    }
    if (!input.pointer.visible) return;
    const bool shooting = input.shootMode || input.shoot.held;
    if (pointer_) {
        // The Wii draws it 22 of 456 lines tall; 16 of 240 stays near that and samples the texture at exactly half.
        starPointer(pointer_, input.pointer.x, input.pointer.y, 16, shooting);
    } else {
        // A diagnostic reticle until the user's original pointer texture is imported.
        const auto c = shooting ? color(255, 204, 112) : color(138, 221, 255);
        C2D_DrawLine(input.pointer.x - 8, input.pointer.y, c, input.pointer.x + 8, input.pointer.y, c, 2, 0);
        C2D_DrawLine(input.pointer.x, input.pointer.y - 8, c, input.pointer.x, input.pointer.y + 8, c, 2, 0);
    }
}

void TouchUi::drawPlayPage(const InputFrame& input, const Settings& settings, bool motionReady, const std::string& summary) {
    const bool stylus = input.pointerMode == PointerMode::Stylus;
    const float opacity = touchButtonOpacity(input);
    if (stylus) {
        label(input.shootMode ? "STAR BITS  /  SHOOT" : "STAR POINTER  /  COLLECT", 126, 12, 0.43f,
              input.shootMode ? color(255, 218, 147) : color(169, 219, 250), true);
        if (!input.pointer.visible) {
            label("touch the stars", 160, 106, 0.59f, color(209, 222, 246, 0.82f), true);
            if (!starBit_ || !camera_) label("original icons awaiting game data", 160, 133, 0.32f, color(139, 156, 192), true);
        } else {
            const float x = input.pointer.x * 319.0f / 399.0f;
            C2D_DrawCircleSolid(x, input.pointer.y, 0, 7, color(195, 221, 255, 0.22f));
            C2D_DrawCircleSolid(x, input.pointer.y, 0, 2, color(222, 238, 255, 0.85f));
        }
        button(ShootButton, input.shootMode, input.shootButtonHeld, opacity, starBit_, "BITS", input.shootMode ? "ON" : "OFF");
        label(input.shootMode ? "touch to shoot" : "touch to collect", 160, 216, 0.33f, color(168, 191, 221, 0.7f), true);
    } else {
        std::string title = std::string("STAR POINTER  /  ") + pointerName(input.pointerMode);
        label(title.c_str(), 126, 12, 0.43f, color(169, 219, 250), true);
        const struct { Action action; const char* name; } rows[]{
            {Jump, "Jump"}, {Spin, "Spin"}, {Crouch, "Crouch"}, {Shoot, "Star Bits"},
            {CameraReset, "Camera"}, {PointerReset, "Recenter"}, {Pause, "Pause"},
        };
        float y = 40;
        for (const auto& row : rows) {
            label(keys(settings.bindings[row.action]).c_str(), 150, y, 0.42f, color(255, 218, 147), true);
            label(row.name, 176, y, 0.42f, color(214, 229, 250));
            y += 19;
        }
        const char* hint = input.pointerMode == PointerMode::CStick ? "C-Stick moves the pointer"
                         : motionReady ? "tilt and turn to aim" : "motion sensors unavailable";
        label(hint, 160, 176, 0.36f, motionReady || input.pointerMode == PointerMode::CStick
              ? color(168, 191, 221, 0.8f) : color(255, 150, 130), true);
        button(RecenterButton, false, input.recenterButtonHeld, 1.0f, nullptr, pointer_ ? "" : "AIM", "CENTER");
        if (pointer_) {
            const float pressed = input.recenterButtonHeld ? 1.0f : 0.0f;
            starPointer(pointer_, RecenterButton.x + RecenterButton.width / 2.0f, RecenterButton.y + 17 + pressed, 22, false);
        }
    }
    button(CameraButton, false, input.cameraButtonHeld, opacity, camera_, "CAM", "RESET");
    chip(OptionsButton, "Options", false, input.optionsButtonHeld);
    if (!stylus && !summary.empty()) label(summary.c_str(), 160, 212, 0.32f, color(139, 156, 192), true);
}

void TouchUi::drawOptionsPage(const InputFrame& input, const Settings& settings) {
    label("OPTIONS", 12, 8, 0.55f, color(236, 243, 255));
    const auto held = [&](Option option) { return input.heldOption == static_cast<int>(option); };
    const auto show = [&](Option option, const char* text) {
        for (const auto& button : OptionButtons)
            if (button.option == option) chip(button.bounds, text, optionActive(option, settings), held(option));
    };
    const struct { const char* name; float y; } rows[]{
        {"Pointer", 36}, {"Motion", 74}, {"C-Stick", 112}, {"Invert", 150}, {"Buttons", 196}};
    for (const auto& row : rows) label(row.name, 10, row.y + 7, 0.44f, color(169, 219, 250));
    show(Option::PointerMotion, "Motion");
    show(Option::PointerCStick, "C-Stick");
    show(Option::PointerStylus, "Stylus");
    char speed[16];
    show(Option::MotionSlower, "-");
    show(Option::MotionFaster, "+");
    std::snprintf(speed, sizeof(speed), "%d%%", settings.motionSpeed);
    label(speed, 200, 81, 0.5f, color(236, 243, 255), true);
    show(Option::StickSlower, "-");
    show(Option::StickFaster, "+");
    std::snprintf(speed, sizeof(speed), "%d%%", settings.stickSpeed);
    label(speed, 200, 119, 0.5f, color(236, 243, 255), true);
    show(Option::InvertX, "Horizontal");
    show(Option::InvertY, "Vertical");
    show(Option::ResetButtons, settings.bindings == defaultBindings() ? "Switch layout" : "Reset layout");
    show(Option::Remap, "Remap...");
    label("also saved as settings.ini", 200, 228, 0.3f, color(139, 156, 192), true);
    show(Option::Done, "Done");
}

void TouchUi::drawRemapPage(const InputFrame& input, const Settings& settings) {
    label("BUTTONS", 12, 8, 0.55f, color(236, 243, 255));
    chip(OptionsButton, "Back", false, input.optionsButtonHeld);
    const char* names[ActionCount]{"Jump", "Spin", "Crouch", "Star Bits", "Camera", "Recenter", "Pause", "Minus"};
    for (unsigned action = 0; action < ActionCount; ++action) {
        const Rect row = remapRow(action);
        const bool choosing = input.capturing == static_cast<int>(action);
        C2D_DrawRectSolid(row.x, row.y, 0, row.width, row.height, choosing ? color(112, 66, 36) : color(22, 36, 70, 0.9f));
        label(names[action], row.x + 8, row.y + 4, 0.42f, color(214, 229, 250));
        const std::string bound = choosing ? "press buttons, then tap" : keys(settings.bindings[action]);
        label(bound.c_str(), row.x + 218, row.y + 4, 0.42f, choosing ? color(255, 231, 176) : color(255, 218, 147), true);
    }
}

void TouchUi::draw(const InputFrame& input, const Settings& settings, const NativePad& pad, SceneRenderer& scene,
                   bool coreReady, const char* assetStatus, bool motionReady) {
    C2D_TextBufClear(text_);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    drawTop(input, pad, scene, coreReady, assetStatus);
    C2D_TargetClear(bottom_, color(4, 9, 26));
    C2D_SceneBegin(bottom_);
    background(320, frame_);
    if (input.remapOpen) drawRemapPage(input, settings);
    else if (input.optionsOpen) drawOptionsPage(input, settings);
    else drawPlayPage(input, settings, motionReady, scene.summary());
    C3D_FrameEnd(0);
    ++frame_;
}
} // namespace smg
