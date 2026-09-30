#pragma once
#include <smg/Archive.hpp>
#include <cstdint>
#include <string>
#include <vector>

class J3DModel;
class XanimeCore;

namespace smg {
// A BMD or BDL's skeleton as JSystem's own J3DModelLoader reads it in place: INF1 (its hierarchy keeping
// only joint nodes), JNT1, EVP1 and DRW1, native-endian and laid out in the loader's structs, which keep
// their offsets in pointer-sized fields. Materials and shapes come with the renderer's side of J3D.
class J3DSkeletonImage {
public:
    bool load(Bytes file, std::string& error);
    const void* data() const { return storage_.data(); }
    std::size_t joints() const { return joints_; }

private:
    std::vector<std::uint64_t> storage_; // eight-byte aligned, as the structs need on 64-bit hosts
    std::size_t joints_ = 0;
};

// JSystem, like the game, leaves what it allocates to the scene's heap: its destructors free nothing. This
// frees a J3DModel made from a skeleton image with one draw matrix buffer, and its model data, as the heap
// going would.
void forgetJ3DSkeleton(J3DModel* model);
// The same for a XanimeCore of its own (not sharing another core's joints).
void forgetXanimeCore(XanimeCore* core);
} // namespace smg
