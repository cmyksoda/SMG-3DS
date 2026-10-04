#include <smg/GxZones.hpp>

namespace smg {
volatile std::uint8_t gxZone = ZoneIdle;

const char* gxZoneName(unsigned zone) {
    static const char* const names[] = {
        "idle",      "lock",      "parse",        "bp",      "call",       "list walk", "layout",
        "xf",        "xf indexed", "list check",  "changed", "state",
        "prepare",   "apply",     "lookup",       "mesh build", "refresh", "uniforms",   "mesh draw", "small",
        "primitive", "cpu vertices", "flush",     "texture", "copy",       "peek",       "frame end", "pause",
        "movie",
    };
    static_assert(sizeof names / sizeof *names == ZoneCount, "a name for every zone");
    return zone < ZoneCount ? names[zone] : "?";
}
} // namespace smg
