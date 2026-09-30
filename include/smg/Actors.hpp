#pragma once
#include <Game/LiveActor/LiveActor.hpp>
#include <JSystem/JGeometry/TMatrix.hpp>
#include <cstddef>

class CollisionParts;

namespace smg {
// A placed part's collision in the original CollisionDirector: a modelless LiveActor with a map-object body
// sensor, as the game's map parts have, holding a CollisionParts made from the part's own KCL and PA.
// The game looks those files up in the actor's ResourceHolder; here the loader hands them over.
class CollisionHost : public LiveActor {
public:
    // kcl and pa stay the caller's, and must outlive the actor; pa may be null.
    CollisionHost(const char* name, const TPos3f& placement, const void* kcl, const void* pa);
    MtxPtr getBaseMtx() const override { return mBase.toMtxPtr(); }

private:
    mutable TPos3f mBase; // the placement's rotation and translation; its scale is in mScale
};

// A diagnostic marker, not Mario: an original LiveActor with no model, falling along the original gravity
// through LiveActor::movement and stopped by the original collision through its Binder.
class ProbeActor : public LiveActor {
public:
    explicit ProbeActor(const TVec3f& position);
    void control() override;
    MtxPtr getBaseMtx() const override;
    bool landed() const { return mLanded; }
    float fallen() const { return mFallen; }

private:
    mutable TPos3f mBase;
    bool mLanded = false;
    float mFallen = 0;
};
} // namespace smg
