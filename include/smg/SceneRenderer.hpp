#pragma once
#include <smg/Input.hpp>
#include <smg/Model.hpp>
#include <citro3d.h>

namespace smg {
class SceneRenderer {
public:
    bool load(const char* archivePath, const char* modelEntry);
    void shutdown();
    void update(const InputFrame& input, float seconds);
    void draw(C3D_RenderTarget* target);
    bool ready() const { return vertices_ != nullptr; }
    const std::string& status() const { return status_; }
private:
    DVLB_s* shader_=nullptr;
    shaderProgram_s program_{};
    bool programReady_=false;
    void* vertices_=nullptr;
    std::vector<ModelShape> shapes_;
    std::vector<ModelMaterial> materials_;
    std::vector<C3D_Tex> textures_;
    C3D_Tex white_{};
    bool whiteReady_=false;
    int projectionLocation_=-1, viewLocation_=-1, colorLocation_=-1;
    C3D_Mtx projection_;
    std::array<float,3> center_{};
    float scale_=1, yaw_=0, pitch_=0.35f;
    std::string status_="No model loaded";
};
}
