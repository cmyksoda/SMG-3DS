#pragma once
#include <smg/Combiner.hpp>
#include <smg/Input.hpp>
#include <smg/Mesh.hpp>
#include <smg/Stage.hpp>
#include <citro3d.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace smg {
// Draws a galaxy's placed models with their translated materials. A worker thread decodes
// models; the main thread only uploads one finished model per frame, since citro3d is not thread-safe.
class SceneRenderer {
public:
    SceneRenderer();
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;
    bool init();
    void shutdown();
    void loadStage(const std::string& root, const std::string& galaxy, int scenario);
    void update(const InputFrame& input, float seconds);
    void draw(C3D_RenderTarget* target);
    bool ready() const { return programReady_ && !instances_.empty(); }
    bool loading() const { return consumed_ < names_.size(); }
    const std::string& status() const { return status_; }
    const std::string& title() const { return title_; }
    const std::string& summary() const { return summary_; } // short enough for the bottom screen

    struct Material;
    struct GpuModel;
    struct Prepared;
private:
    struct Instance {
        GpuModel* model;
        C3D_Mtx transform;
        float center[3], radius;
        bool sky;
    };
    static void workerMain(void* self);
    void work();
    void stopWorker();
    void consume(Prepared& prepared);
    void resetCamera();
    void drawInstance(const Instance& instance, const C3D_Mtx& view, bool translucent, const Material*& current);

    DVLB_s* shader_ = nullptr;
    shaderProgram_s program_{};
    bool programReady_ = false;
    int projection_ = -1, modelView_ = -1, materialColor_ = -1, ambientColor_ = -1;
    int lightDirection_ = -1, lightColor_ = -1, colorSelect_ = -1, texgen_[3]{-1, -1, -1};
    std::string root_, title_, status_ = "No stage loaded", summary_;
    Stage stage_;
    std::vector<std::string> names_;
    std::size_t consumed_ = 0, skinned_ = 0, missing_ = 0, failed_ = 0;
    Thread worker_ = nullptr;
    std::atomic<bool> stopping_{false};
    LightLock lock_;
    std::vector<std::unique_ptr<Prepared>> finished_; // guarded by lock_
    std::vector<std::unique_ptr<GpuModel>> models_;
    std::vector<Instance> instances_;
    float position_[3]{}, yaw_ = 0, pitch_ = 0;
};
} // namespace smg
