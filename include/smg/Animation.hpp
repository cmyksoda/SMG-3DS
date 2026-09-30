#pragma once
#include <smg/Archive.hpp>
#include <smg/Model.hpp>
#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <string>
#include <vector>

namespace smg {
// A BRK (J3D TEV register animation), validated and copied into native tables for the original
// J3DAnmTevRegKey, which J3DAnmKeyLoader_v15 would point straight into the big-endian file.
class TevRegAnimation {
public:
    TevRegAnimation() = default;
    TevRegAnimation(const TevRegAnimation&) = delete;
    TevRegAnimation& operator=(const TevRegAnimation&) = delete;

    bool load(Bytes source, std::string& error);
    const J3DAnmTevRegKey& key() const { return key_; }
    // J3DTevColorAnm and J3DTevKColorAnm at a frame: each animated register of the named materials.
    // Returns how many registers it set.
    unsigned apply(float frame, std::vector<ModelMaterial>& materials);

private:
    J3DAnmTevRegKey key_;
    std::vector<J3DAnmCRegKeyTable> colorTables_;
    std::vector<J3DAnmKRegKeyTable> konstTables_;
    std::vector<s16> values_[8]; // color R, G, B, A, then konst R, G, B, A
    std::vector<std::string> colorNames_, konstNames_;
};
} // namespace smg
