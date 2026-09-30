#include <smg/Actors.hpp>
#include <Game/LiveActor/Binder.hpp>
#include <Game/Map/CollisionParts.hpp>
#include <Game/Scene/SceneFunction.hpp>
#include <Game/Util/ObjUtil.hpp>
#include <Game/Util/ActorSensorUtil.hpp>
#include <Game/Util/LiveActorUtil.hpp>
#include <cmath>

namespace smg {
CollisionHost::CollisionHost(const char* name, const TPos3f& placement, const void* kcl, const void* pa) : LiveActor(name) {
    // MR::makeMtxTRS takes the matrix apart the same way: scale per column, then rotation and translation.
    float scale[3];
    mBase = placement;
    for (int column = 0; column < 3; ++column) {
        scale[column] = std::sqrt(placement.mMtx[0][column] * placement.mMtx[0][column] + placement.mMtx[1][column] * placement.mMtx[1][column] +
                                  placement.mMtx[2][column] * placement.mMtx[2][column]);
        for (int row = 0; row < 3; ++row) mBase.mMtx[row][column] /= scale[column] > 0 ? scale[column] : 1;
    }
    mScale.set(scale[0], scale[1], scale[2]);
    mPosition.set(placement.mMtx[0][3], placement.mMtx[1][3], placement.mMtx[2][3]);
    initHitSensor(1);
    MR::addHitSensorMapObj(this, "body", 8, 0.0f, TVec3f(0.0f, 0.0f, 0.0f));
    // LiveActor::initActorCollisionParts through MR::createCollisionPartsFromLiveActor: the TRS matrix, the
    // plain scale type, and the part left invalid until the actor appears.
    auto* parts = new CollisionParts();
    parts->init(placement, getSensor("body"), kcl, pa, 0, false);
    mCollisionParts = parts;
    MR::invalidateCollisionParts(this);
    makeActorAppeared();
}

ProbeActor::ProbeActor(const TVec3f& position) : LiveActor("probe") {
    mPosition = position;
    initHitSensor(1);
    MR::addHitSensorBinder(this, "body", 8, 20.0f, TVec3f(0.0f, 0.0f, 0.0f));
    initBinder(20.0f, 0.0f, 0);
    MR::onCalcGravity(this);
    MR::connectToScene(this, MR::MovementType_MapObj, MR::CalcAnimType_None, MR::DrawBufferType_None, MR::DrawType_None);
    makeActorAppeared();
    // A marker has to keep falling wherever the camera looks.
    MR::invalidateClipping(this);
}

void ProbeActor::control() {
    // A fixed pull and top speed per frame for a marker; the game's own actors each choose theirs.
    constexpr float Acceleration = 1.0f, TopSpeed = 60.0f;
    if (MR::isOnGround(this)) {
        // Standing: only this frame's pull remains, pressing into the ground for the Binder to answer.
        mVelocity = mGravity * Acceleration;
        mLanded = true;
        return;
    }
    mVelocity += mGravity * Acceleration;
    if (mVelocity.length() > TopSpeed) mVelocity.setLength(TopSpeed);
    mFallen += mVelocity.length();
}

MtxPtr ProbeActor::getBaseMtx() const {
    mBase.identity();
    mBase.setTrans(mPosition);
    return mBase.toMtxPtr();
}
} // namespace smg
