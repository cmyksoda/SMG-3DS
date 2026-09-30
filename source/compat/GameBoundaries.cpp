// Entry points of game classes whose sources the whole-game library (petari_game) builds. Programs without it
// (the preview, the tests and tools) link these instead, and each stops the program by name if reached.
// See docs/PORTING.md, "Road to Mario".
#include <Game/LiveActor/ActorAnimKeeper.hpp>
#include <Game/LiveActor/ActorLightCtrl.hpp>
#include <Game/LiveActor/ActorPadAndCameraCtrl.hpp>
#include <Game/LiveActor/EffectKeeper.hpp>
#include <Game/LiveActor/LiveActor.hpp>
#include <Game/LiveActor/LodCtrl.hpp>
#include <Game/LiveActor/ModelManager.hpp>
#include <Game/LiveActor/ShadowController.hpp>
#include <Game/Map/CollisionParts.hpp>
#include <Game/Screen/StarPointerTarget.hpp>
#include <Game/Util/LiveActorUtil.hpp>
#include <Game/Util/MemoryUtil.hpp>
#include <Game/Util/ModelUtil.hpp>
#include <Game/Util/PlayerUtil.hpp>
#include <JSystem/J3DGraphBase/J3DShape.hpp>
#include <cstdio>
#include <cstdlib>

namespace {
[[noreturn]] void unported(const char* what) {
    std::fprintf(stderr, "Not brought up yet: %s\n", what);
    std::abort();
}
} // namespace

// Models and their animation: ModelManager, J3DModel and the animation keepers come with milestone 2.
ModelManager::ModelManager() { unported("ModelManager"); }
void ModelManager::init(const char*, const char*, bool) { unported("ModelManager::init"); }
void ModelManager::update() { unported("ModelManager::update"); }
void ModelManager::calcAnim() { unported("ModelManager::calcAnim"); }
void ModelManager::calcView() { unported("ModelManager::calcView"); }
ActorAnimKeeper* ActorAnimKeeper::tryCreate(LiveActor*) { unported("ActorAnimKeeper::tryCreate"); }
void ActorAnimKeeper::update() { unported("ActorAnimKeeper::update"); }
ActorPadAndCameraCtrl* ActorPadAndCameraCtrl::tryCreate(const ModelManager*, const TVec3f*) {
    unported("ActorPadAndCameraCtrl::tryCreate");
}
void ActorPadAndCameraCtrl::update() { unported("ActorPadAndCameraCtrl::update"); }
namespace MR {
J3DModel* getJ3DModel(const LiveActor*) { unported("MR::getJ3DModel"); }
void calcJ3DModel(LiveActor*) { unported("MR::calcJ3DModel"); }
ResourceHolder* getModelResourceHolder(const LiveActor*) { unported("MR::getModelResourceHolder"); }
// The actor's resources come with its model; the native scene hands map parts their collision directly.
CollisionParts* createCollisionPartsFromLiveActor(LiveActor*, const char*, HitSensor*, CollisionScaleType) {
    unported("MR::createCollisionPartsFromLiveActor");
}
CollisionParts* createCollisionPartsFromLiveActor(LiveActor*, const char*, HitSensor*, MtxPtr, CollisionScaleType) {
    unported("MR::createCollisionPartsFromLiveActor");
}
CollisionParts* createCollisionPartsFromResourceHolder(ResourceHolder*, const char*, HitSensor*, const TPos3f&, CollisionScaleType) {
    unported("MR::createCollisionPartsFromResourceHolder");
}
// JKRHeap's current heap: the port allocates from the ordinary heap.
JKRHeap* getCurrentHeap() { unported("MR::getCurrentHeap"); }
// ActorLightCtrl draws with the draw buffers; the preview lights models from the same tables itself.
void updateLightCtrl(LiveActor*) { unported("MR::updateLightCtrl"); }
} // namespace MR
// Model LOD comes with models; view groups follow the player, who comes with Mario.
void LodCtrl::setViewCtrlPtr(const bool*, const bool*, const bool*, const bool*) { unported("LodCtrl::setViewCtrlPtr"); }
namespace MR {
TVec3f* getPlayerPos() { unported("MR::getPlayerPos"); }
} // namespace MR
ActorLightCtrl::ActorLightCtrl(const LiveActor*) { unported("ActorLightCtrl"); }
void ActorLightCtrl::reset() { unported("ActorLightCtrl::reset"); }

// Effects, shadows and the star pointer come with their directors; LiveActor's sound setup is left out of it.
EffectKeeper::EffectKeeper(const char*, ResourceHolder*, int, const char*) { unported("EffectKeeper"); }
void EffectKeeper::init(LiveActor*) { unported("EffectKeeper::init"); }
void EffectKeeper::setBinder(const Binder*) { unported("EffectKeeper::setBinder"); }
void EffectKeeper::update() { unported("EffectKeeper::update"); }
void EffectKeeper::stopEmitterOnClipped() { unported("EffectKeeper::stopEmitterOnClipped"); }
void EffectKeeper::playEmitterOffClipped() { unported("EffectKeeper::playEmitterOffClipped"); }
void EffectKeeper::clear() { unported("EffectKeeper::clear"); }
void EffectKeeper::enableSort() { unported("EffectKeeper::enableSort"); }
namespace MR {
// SoundUtil's version sets the object's floor code and plays its animation sounds; with no sound object it
// returns at once, and no actor has one until sound comes up.
void actorSoundMovement(LiveActor* pActor) {
    if (pActor->mSoundObject != nullptr) unported("MR::actorSoundMovement");
}
} // namespace MR
ShadowControllerList::ShadowControllerList(LiveActor*, u32) { unported("ShadowControllerList"); }
void ShadowControllerList::resetCalcCount() { unported("ShadowControllerList::resetCalcCount"); }
void ShadowControllerList::requestCalc() { unported("ShadowControllerList::requestCalc"); }
StarPointerTarget::StarPointerTarget(f32, const TVec3f*, MtxPtr, TVec3f) { unported("StarPointerTarget"); }

// The game's own replacement for this J3D draw step lives in Game/System/Overwrite.cpp.
void J3DShapeMtx::loadMtxIndx_PNGP(int, u16) const { unported("J3DShapeMtx::loadMtxIndx_PNGP"); }
