#pragma once
#include <cstdint>

namespace smg {
// What the render thread is doing, as one byte it writes as it goes; with dev.gx_zones a thread on another core reads
// it now and then, which costs the renderer nothing (a clock read costs Azahar 150 cycles, so timing skews what it times).
enum GxZone : std::uint8_t {
    ZoneIdle,         // waiting for the game's stream
    ZoneLock,         // waiting for the processor's lock
    ZoneParse,        // the command processor's own reading
    ZoneBp,           // a run of BP loads
    ZoneCall,         // a display list called
    ZoneListWalk,     // a called list read command by command
    ZoneLayout,       // a vertex format's layout read
    ZoneXf,           // XF loads from the stream
    ZoneXfIndexed,    // XF loads from arrays (matrices, lights)
    ZoneListCheck,    // a called list: whether its mesh can draw it
    ZoneChanged,      // memory the game changed, matched against meshes
    ZoneState,        // the draw state found or built
    ZonePrepare,      // the vertex pipeline prepared for a state
    ZoneApply,        // a state's pixel settings and textures given to citro3d
    ZoneMeshLookup,   // a mesh found by its key
    ZoneMeshBuild,    // a mesh made
    ZoneMeshRefresh,  // a moving mesh decoded again
    ZoneUniforms,     // a mesh draw's uniforms
    ZoneMeshDraw,     // a mesh draw's citro3d state and draws
    ZoneSmallList,    // a small list drawn from its decoded copy
    ZonePrimitive,    // a CPU-path primitive taken in
    ZoneCpuVertices,  // CPU-path vertices transformed and lit
    ZoneFlush,        // a CPU-path batch drawn
    ZoneTexture,      // a texture decoded and converted
    ZoneCopy,         // an EFB copy
    ZonePeek,         // a pixel read back for the game
    ZoneFrameEnd,     // a frame ended and shown, the GPU waited for
    ZonePause,        // a frame ended and begun again unshown, the GPU waited for
    ZoneMovie,        // a movie frame
    ZoneCount
};

extern volatile std::uint8_t gxZone;
const char* gxZoneName(unsigned zone);

class GxZoneScope {
public:
    explicit GxZoneScope(GxZone zone) : previous_(gxZone) { gxZone = zone; }
    ~GxZoneScope() { gxZone = previous_; }
    GxZoneScope(const GxZoneScope&) = delete;
    GxZoneScope& operator=(const GxZoneScope&) = delete;

private:
    std::uint8_t previous_;
};
} // namespace smg
