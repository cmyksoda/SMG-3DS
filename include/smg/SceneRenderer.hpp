#pragma once
#include <smg/Combiner.hpp>
#include <smg/Input.hpp>
#include <smg/Mesh.hpp>
#include <smg/Stage.hpp>
#include <citro3d.h>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct AreaLightInfo;

namespace smg {
class NativeScene;
class StageCollision;

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
    // What the original gravity code pulls with at Mario's start, and where the camera is now.
    const std::string& startGravity() const { return startGravity_; }
    const std::string& gravityHere() const { return gravityHere_; }
    // Where that pull meets the placed models' original collision.
    const std::string& startLanding() const { return startLanding_; }

    // A diagnostic marker, not Mario: dropped above Mario's start, it falls along the original gravity and
    // stops on the original collision. Positions are on the top screen, from the last draw.
    struct Probe {
        bool visible = false;
        float x = 0, y = 0;
        std::vector<std::array<float, 2>> trail; // oldest first, only the points in view
        std::string label;
    };
    const Probe& probe() const { return probeView_; }

    struct Material;
    struct GpuModel;
    struct Prepared;
private:
    struct Instance {
        GpuModel* model;
        C3D_Mtx transform;
        float center[3], radius;
        bool sky;
        int light; // index into lightSets_
    };
    // An area light's planet set, as the director loads it for a draw buffer.
    struct LightSet {
        const AreaLightInfo* area;
        struct Light {
            float color[4]{}, position[3]{};
            bool followCamera = false;
        } lights[2];
        float ambient[4]{1, 1, 1, 1}, alpha = 0;
    };
    static void workerMain(void* self);
    void work();
    void stopWorker();
    void consume(Prepared& prepared);
    void resetCamera();
    void drawInstance(const Instance& instance, const C3D_Mtx& view, bool translucent, const Material*& current);
    int lightSet(const AreaLightInfo* area);
    void loadLightSet(const LightSet& set, const C3D_Mtx& view);
    void dropProbe();
    void stepProbe(float seconds);

    DVLB_s* shader_ = nullptr;
    shaderProgram_s program_{};
    bool programReady_ = false;
    int projection_ = -1, modelView_ = -1, materialColor_ = -1, lightAmbient_ = -1, light0Pos_ = -1, light0Color_ = -1;
    int light1Pos_ = -1, light1Color_ = -1, light2Alpha_ = -1, colorSelect_ = -1, alphaSelect_ = -1, colorLights_ = -1;
    int alphaLights_ = -1, texgen_[3]{-1, -1, -1};
    std::vector<LightSet> lightSets_; // the first is the stage's default; without light data, plain white
    int currentLight_ = -1;
    std::string lightError_;
    std::string root_, title_, status_ = "No stage loaded", summary_, startGravity_, gravityHere_, startLanding_;
    Stage stage_;
    std::unique_ptr<NativeScene> gravity_;
    std::unique_ptr<StageCollision> collision_; // after the scene, so it goes first
    std::vector<std::string> names_;
    std::size_t consumed_ = 0, skinned_ = 0, missing_ = 0, failed_ = 0;
    Thread worker_ = nullptr;
    std::atomic<bool> stopping_{false};
    LightLock lock_;
    std::vector<std::unique_ptr<Prepared>> finished_; // guarded by lock_
    std::vector<std::unique_ptr<GpuModel>> models_;
    std::vector<Instance> instances_;
    float position_[3]{}, yaw_ = 0, pitch_ = 0;
    bool probeFalling_ = false;
    float probe_[3]{}, probeVelocity_[3]{}, probeFallen_ = 0, probeSinceTrail_ = 0;
    std::vector<std::array<float, 3>> probeTrail_;
    std::string probeState_;
    Probe probeView_;
};
} // namespace smg
