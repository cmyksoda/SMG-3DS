#pragma once

#include <JSystem/J3DGraphAnimator/J3DAnimation.hpp>
#include <revolution/mtx.h>

// PSMTXConcat(a, b, out), then PSMTXCopy(out, copy), the product stored from the registers into both.
extern "C" void smgMTXConcatTee(const Mtx a, const Mtx b, Mtx out, Mtx copy);

// A joint's transform from a track's animation. A key animation (kind 8) reads each channel from where it last did.
inline void smgGetTransform(J3DAnmTransform* anm, u16 joint, J3DTransformInfo* out, u16* hints) {
    if (anm->getKind() == 8) {
        static_cast< J3DAnmTransformKey* >(anm)->calcTransformHinted(anm->getFrame(), joint, out, hints);
    } else {
        anm->getTransform(joint, out);
    }
}
