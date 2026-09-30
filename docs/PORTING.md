# Engine integration

Upstream: `SMGCommunity/Petari` at `31bacf2e4f9788f50c1e7b2e501b3a66da9dd8f2`.
The unmodified checkout is a pinned git submodule. Its CC0 notice remains in
`upstream/Petari/LICENSE`. Credit the Petari contributors for the decompilation.

The upstream build reconstructs a Wii executable using Metrowerks and the RMGK01
configuration. Decompiled code still includes PowerPC instructions, Wii SDK calls,
big-endian file assumptions, and GX command-stream operations. Decompilation
completion does not make those interfaces portable to ARM11/PICA200.

## Integrated in the native bootstrap

- Original `Nerve`, `Spine`, `ActorStateBase`, `ActorStateKeeper`, `NerveExecutor`,
  `TriggerChecker`, `WPadButton`, `WPadStick`, and JGadget `hashcode` translation units
  compile for ARM11 without editing their sources.
- Original `JMapInfo` compiles through generated GCC-compatible source/header copies. These fix
  switch/goto initialization scopes, inline definitions, and unnecessarily broad SDK includes.
  `Bcsv` validates and repacks raw Wii records into aligned native-endian storage before
  attaching that storage to the original reader. Tests exercise its actual lookup functions.
- `compat/revolution/types.h` supplies fixed-width types without redefining C++ keywords.
- `Input` samples logical actions through Petari's `TriggerChecker`; `toWiiInput`
  translates actions to the original Wii button bitmasks and explicit shake state.
- `NativePad` supplies bounded KPAD samples to the original button and stick classes;
  `MR::getWPad` resolves the owned controller. The diagnostic displays their actual output.
  Repeat flags use elapsed time without fabricating button edges. Digital spin and stylus
  data remain explicit boundary values until the swing/pointer and GamePadUtil work is finished.
- Generated math headers supply real non-Metrowerks implementations for vector copying,
  assignment, dot products, square roots, and Hermite interpolation. Scalar vector/quaternion
  helpers preserve in-place calls. The original trig tables run with a GCC declaration fix
  and an explicit endpoint lookup instead of indexing beyond an array into its next member.
- Two native render targets display the stage preview and the bottom-screen controls.
  `Settings` holds the remappable Switch-style layout and pointer options; `MotionTracker`
  drives the pointer from the gyro in player space. Stylus gesture ownership prevents UI
  presses from generating shots and allows aiming across 50%-transparent buttons.
- Yaz0 decompression and RARC indexing work independently of host endianness and alignment.
  Archives are found whatever the case of their names, as the Wii's DVD file system finds them:
  the disc stores `FloaterOtakingGalaxy.arc` and `CubeBubbleEXAZone.arc` where the game asks for
  `FloaterOtaKingGalaxy` and `CubeBubbleExAZone`. A 3DS SD card ignores case already; a Linux
  host, or Azahar's SD folder on one, needs each part of the path matched.
  The bootstrap probes the original star-bit layout archive and the Good Egg scenario table
  if they are present on the SD card.
- The J3D reader loads every BMD/BDL on the disc: joint hierarchies, envelope skinning
  baked into the bind pose, per-packet matrix slots, two UV sets, all GX texture formats
  with their palettes, and the complete MAT3 material state.
- `compileTev` translates each material's TEV program into at most six PICA200 combiner
  stages and three texture units, reporting when and why it has to approximate. Host and
  on-ARM tests compare it against reference GX and PICA evaluators. The GX reference follows
  Dolphin's model of the hardware: inputs a, b and c see a register's low eight bits, d sees its
  whole signed value, a stage saturates only when its clamp bit is set, and the output is taken
  modulo 256. A third of the disc's materials read a signed register as d (Good Egg's start planet
  keep -0.5 in C0's alpha); a constant clamped to zero there turned whole planets white.
  Range tracking marks programs where an unclamped value would need more than 0..1. When the last
  stage's unclamped sum provably stays within one window of 256 steps without reaching the eleven-bit
  limit, the wrap is folded into its constant d and the program stays exact. Mario's eyelid depends on
  it: 1 + texA/4 + 1/2 wraps to 127 where the lid's texture is clear, just under its alpha test's 128,
  so the eye shows through. A register whose value every input fixed can be read as a constant; that is
  tried when reading the previous value is not exact, since it costs a constant slot.
- The original `MathUtil`, `MtxUtil` and nine gravity shapes (point, parallel, cube, disk,
  disk torus, cone, segment, wire and the `PlanetGravity` base) compile for ARM from
  generated copies that include only what each file uses. Three helpers whose only bodies
  were paired-single assembly, and `fastSqrtf`/`frsqrte`, have scalar bodies; the SDK matrix
  and quaternion routines they call have portable implementations. `MR::getRandom` reads a
  natively held `TRandom_fast_` seeded as `GameSystemObjHolder` seeds it. Every shape is
  under test: point, parallel, disk, segment and wire directly, and cube, cone, disk torus and wire
  through their original creators, fed `planetobjinfo` rows (and, for wire, a path table).
- The original `NameObj`, `NameObjRegister`, `NameObjHolder` and `NameObjGroup` compile for ARM,
  with `MR::getHashCode` from `HashUtil` (MSL's locale table replaced by the "C" locale's ASCII
  lowering it holds) and ObjUtil's and SystemUtil's NameObj helpers copied out of their files
  verbatim by `tools/prepare-petari.py`. Their scene is the native scene described below. Plain
  `char` is unsigned on every build, as on the Wii and the 3DS, so Shift-JIS names hash the same.
- The original `PlanetGravityManager` and `GravityInfo` combine every registered gravity by
  priority, type, host and switch state. Hosts are compared as `u32`, which holds a pointer on
  the Wii and the 3DS; 64-bit host tests compare its low half.
- Placed gravity is built by the original code: `JMapUtil` (the whole file, with narrow includes),
  `GravityUtil` (without its seven `LiveActor` overloads, left out with a comment until actors run)
  and `GravityCreator`, whose wire creator samples a rail through the original `RailRider`,
  `BezierRail` and `RailPart`. The native scene answers `MR::getRailInfo` as
  `StageDataHolder::getCommonPathPointInfo` does, from each zone's `CommonPathInfo` and
  `CommonPathPointInfo.<n>` tables; `RailRider`'s camera-rail constructor waits for the camera.
  `placeStageGravity` walks a scenario's zones and active layers and hands every `planetobjinfo`
  row to the native scene, which does what `GlobalGravityObj` does (see below). For Good Egg
  scenario 1 that is 111 gravities in 16 zones. `gravity_tool` asks a stage for the pull at any
  point and follows it to the first placed model: from scenario 1's start, a disk gravity in
  `DiskGardenZone` pulls straight down onto `DiskGardenPlanet`, 9.2 units below. The preview
  shows the same answer at Mario's start and, live, at the camera. A gravity probe, a magenta
  marker drawn over the scene and plainly not Mario, drops 800 units up the pull at Mario's start
  (again with L). It is an original `LiveActor` with no model (`smg::ProbeActor`), run by the scene's
  movement: `LiveActor::movement` takes its gravity from the original manager, its `control` adds a
  fixed unit per frame along it, and its original `Binder` stops it on the original `CollisionParts`
  of every placed part.
- Collision runs through the original `KCollisionServer` (`KCollision`, `KCollisionPlus`) and
  `CollisionCode`. `smg::Kcl` validates a disc KCL and repacks it, as `Bcsv` repacks tables: every
  field swapped by its type (octree nodes are found by walking the tree the way `searchBlock` does;
  what they point at are u16 prism lists), and a header in this build's own `KCLFile` layout, so
  `setData` can turn offsets into 64-bit pointers on the host. Three adaptations: the Wii's
  `isBinaryInitialized` asked whether the first header word was negative, true only for Wii pointers,
  so it now asks whether it points into the image; `searchBlock` shifted by -1 before checking for a
  single-block file, undefined in C++, so it checks first; and one `goto` crossed an initialization.
  `calcFarthestVertexDistance` reads camera ids through `CollisionDirector`'s `CollisionCode`, which the
  native scene holds, and registers them with `CameraDirector`, which the native scene collects for.
  `CollisionParts` owns a server per placed part but is created by actors (it takes a `HitSensor` whose
  host is a `LiveActor`), so it waits for them. Until then `smg::StageCollision` gives each placement
  of a model with a `<name>.kcl` its own server and does for every part what
  `CollisionParts::checkStrikeLine` does. From Mario's start in Good Egg scenario 1 the pull lands on
  `DiskGardenPlanet`'s collision 13.9 units below, on a Normal floor (the drawn grass is 9.2 below).
- TEV register animation (BRK) is evaluated by JSystem's own `J3DAnmTevRegKey` and
  `J3DGetKeyFrameInterpolation`, copied out of `J3DAnimation.cpp`. `J3DAnmKeyLoader_v15` points those
  objects straight into the big-endian file; `smg::TevRegAnimation` validates the TRK1 block and gives
  them native copies instead. Three SDK and compiler gaps had no GCC body: the `s16` Hermite overload
  and `__OSf32tos16`/`__OSf32tou8` were Metrowerks assembly only (the fast casts would have returned
  garbage). `OSInitFastCast` sets the quantization registers to plain integers, and `psq_st` truncates
  and saturates, so the port converts and clamps the same way. All 503 BRK files on the disc load and
  name materials of their archive's models. Map parts take the BRK named after them at frame 0, as
  `MapObjActor::init`'s `tryStartAllAnim` starts it; a warp pipe stops at its color, `Obj_arg7`
  (Good Egg's are green at 0 and orange at 1). Playing a BRK over time waits for frame control.
- Joint animation (BCK) runs through JSystem's own `J3DAnmTransformKey::calcTransform` and
  `J3DFrameCtrl`, from `smg::JointAnimation`'s native copies of the ANK1 block. Rotations are often
  negative and `calcTransform` shifts them left, which C++ leaves undefined before C++20 (PowerPC's
  `slw` just scales); the copy multiplies by the same power of two. `loadModel` can keep what the
  bind pose bakes away (each vertex before its draw matrix, the draw matrices' joints and weights,
  the inverse binds), and `poseSkeleton` and `skinModel` pose and skin on the CPU. Every one of the
  disc's 1,758 models, reposed at its own bind pose, skins back to exactly what the loader baked,
  and all 3,231 BCK files load and pose to finite values on every frame.
  The preview stands Mario's own `mario.bdl` where his start's pull meets the collision, upright
  against that pull and facing the start's way, lit by the area's player set (`ActorLightCtrl`'s
  type 0), and plays `MarioAnime`'s `wait.bck` in a `J3DFrameCtrl` at its own play mode, one
  frame per game frame. The CPU skins only the packed vertices (`packMesh` keeps vertices on
  different draw matrices apart and says which model vertex each came from) into one of two
  linear buffers while the GPU reads the other. This is a model and an animation, not the player:
  `MarioActor` would hide the body's `Face0` joint for `MarioFace` and swap `MarioHandL`/`R` poses
  by the `PartsControl` joint, and `XanimePlayer` would blend animations; none of that runs yet.
- newlib's `<ctype.h>` defines `_U`, `_L`, `_N`, `_S`, `_P`, `_C`, `_X` and `_B` as macros, and
  Petari uses some of them as member names. ARM builds force-include `compat/smg/NewlibCtype.hpp`,
  which lets libstdc++ read them once and then undefines them.
- `NameObjCategoryList`, the lists the scheduler runs movement, calcAnim and draw through,
  compiles with one adaptation: the game calls every list's delegator through the non-const
  type, const draw lists included, which is undefined behavior UBSan reports; the two delegator
  types now share a base. `NameObjListExecutor` itself also owns the LiveActor draw buffers,
  so it waits until actors draw.
- JKRHeap's placement `operator new` forms allocate from the C++ free store, which stands in for
  the current heap. A request naming a `JKRHeap`, or wanting more alignment than the free store
  gives, stops the program with a message: neither exists in the port yet.
- `loadStage` resolves a galaxy's scenario, zones and layers into world-space placements.
  The renderer streams those models in one per frame, packs them into indexed 36-byte
  vertices and native texture formats, and draws them with the translated combiners, GX's
  color and alpha channels lit as the game lights its planets, three texgens, blending, alpha test
  and depth modes. The original `LightDataHolder` and `LightZoneDataHolder` read LightData.arc
  (their director and archive are the native scene's), and the stage's default area light (LightID
  -1 in the galaxy's own table) gives its planet set: two diffuse lights, a light at the camera
  that lights only alpha, which the planets' TEV programs turn into rim light, and an ambient,
  which lit channels use in place of the material's own (a judgment call: the director loads it
  for every draw buffer). Point lights come with actors, and so do the player, enemy and NPC sets.
  Light areas are the original `LightArea`, `AreaObj` and `AreaForm`,
  placed from `LightCtrlCube` and `LightCtrlCylinder` rows as `NameObjFactory` makes them; each
  placement takes the light of the area around its origin, as `ActorLightCtrl::init` finds it. In Good
  Egg the planets' sets barely differ between areas (the areas change the player and enemy sets), so
  the look changes little until actors draw. Petari's string literals are UTF-8 in this build and the disc's
  names Shift-JIS, so `LightDataHolder`'s "デフォルト" never matches a row by name; its fallback, the
  first row, is the one the disc names that anyway. Skinned placed models are still skipped: which
  animation each plays comes with its actor. This is a stage preview with a free camera, not gameplay:
  no actor or camera code from the game runs yet, and its gravity and collision answer only the
  readouts, the probe and where Mario's model stands.

The WPad pointer, accelerometer, swing, and rumble device members are not installed yet.
Do not compile callers that dereference them until their native implementations exist.
No silent Wii SDK stubs are used to make gameplay appear to work. Native WPadReadDataInfo
borrows its owner's sample storage rather than allocating the Wii's 120-sample IR buffer.

## Next engine work

1. **Portable math and SDK surface.** Audit every Metrowerks-only function before using it.
   The first vector/interpolation paths are implemented and tested; matrices, projection,
   and the remaining SDK functions still need real replacements. Scalar/VFP math does not
   reproduce Gekko estimate rounding bit for bit. Preserve original coordinate and matrix
   conventions, and validate camera/collision behavior with real stages. Other uncompiled
   headers may still contain empty non-Metrowerks branches.
2. **Endianness and resource loading.** Connect SD-backed archives to JKR resource lookup.
   Validate the BCSV/JMap adapter on actual game data, then adapt BMD/BDL, animation,
   collision, effects, layouts, and audio readers;
   direct casts of Wii bytes to ARM structures are insufficient. Handle the game's Shift-JIS
   names and text encodings deliberately.
3. **PICA200 renderer.** The static stage path exists. Still missing: point lights, GX's second
   color channel, projection texgens (they
   fall back to sphere maps), mipmaps, BTK and BPK animation and BRK playback over time, billboards, runtime
   skinning of placed models, fog, particles and postprocessing. `resource_audit --models` lists the 793
   approximated TEV programs by reason: more than two distinct constants (314) and an unclamped
   output or value outside 0..1 (228 and 156, which the combiner saturates where GX would wrap or
   carry it) lead.
   A loader thread decodes models while the main thread uploads one per frame. Textures go in upside
   down: GX reads v down from an image's first row, the PICA reads t up from the last row in memory.
   Tiling stage textures hid the flip until Mario's cap emblem read "W". CMPR
   textures become ETC1/ETC1A4 (a GPU readback test confirms the block layout), leaving
   19.4 MB of linear memory after Good Egg in Azahar; other formats stay uncompressed.
4. **Original scene and actor execution.** Placed gravity runs through the original creators
   over the native scene. Next: `LiveActor` (which also brings back `GlobalGravityObj` and the
   `GravityUtil` actor overloads, and `CollisionParts` with `CollisionDirector`'s keepers), then the
   original `NameObjListExecutor` scheduler,
   gravity, collision, stage placement, Mario, animation, and camera using the real resources.
   Integrate the input boundary into `GamePadUtil`/`WPad`, including explicit digital spin input.
   Skip the Wii pointer's multi-frame loss-of-IR delay: stylus lift must hide it immediately.
   The Switch release's right-stick camera needs new camera code; the C-Stick value is ready.
5. **Complete gameplay.** Stage transitions, menus, save migration/format, audio through NDSP,
   cutscenes, effects, motion-controlled minigames, and all game-specific exceptions.
6. **Hardware validation and optimization.** Frame time, draw count, texture/material limits,
   heap peaks, streaming stalls, suspend/resume, and input accuracy on a real 3DS.

The New Nintendo 3DS and New 2DS family is the target: the controls need ZL, ZR and the
C-Stick, and the faster CPU and larger memory are what full Galaxy needs most. Nothing so
far establishes that full gameplay fits or runs acceptably even there.

## Bringing up NameObj and the scene

Placed gravity, collision and every actor sit on `NameObj`. Its 13 sources split cleanly:

| Slice | Needs |
| --- | --- |
| `NameObj`, `NameObjRegister`, `NameObjHolder`, `NameObjGroup` (running) | `SingletonHolder`, `MR::getHashCode`, movement on/off requests |
| `NameObjCategoryList` (running) | `MR::FunctorBase` - the movement, calcAnim and draw lists |
| `NameObjListExecutor` | `DrawBufferHolder`: the LiveActor draw buffers, J3D model drawing and light lookup |
| `NameObjExecuteHolder`, `NameObjFinder` | `GameSystem`, `GameSystemSceneController`, `SceneObjHolder` |

Three ways forward:

1. **Compile the scene system too.** `GameSystem`, its scene controller and `SceneObjHolder`
   come from `Game/System` (99 sources) and pull in JKernel heaps, threads, audio and the
   layout system. Most faithful; weeks of bring-up before anything placed runs.
2. **Native scene everywhere.** Reimplement the holder and scheduler natively. Fastest,
   but it grows into a second engine that drifts from the original.
3. **Original objects, native holder (chosen 2026-09-29).** Compile the first slice and the
   scheduler, and give them a small native `SceneObjHolder` that owns only what the port
   has brought up - `PlanetGravityManager` first, so `GravityCreator` can build a zone's
   real gravity from `planetobjinfo`. It is an explicit, removable boundary like
   `smgGameRandom()`, replaced by the original once `GameSystem` runs.

## Road to Mario

Measured 2026-09-30. The decompilation is complete: every function the Korean build's symbol map places
in `Game/Player` has source, and so does nearly everything else. The work left is porting, not waiting.

**What GCC already accepts.** A syntax-only pass of all 1,605 `src/Game` files with devkitARM's
`arm-none-eabi-g++` and the port's include order (`tools/petari-syntax-scan.sh BUILD_DIR OUT.tsv`)
first compiled 830 unchanged (52%). On the 3DS `u32`
is `unsigned long` as on the Wii, so a 64-bit host's mismatches (`JKRDvdRipper`, pointer casts) are
not target problems. The failures clustered on a few shared headers, and five adaptations took the
count to 1,208 (75%): Camera 126 of 127, Gravity 14 of 14, MapObj 266 of 317, Player 82 of 107. Giving
the compat `types.h` back upstream's utility macros (`ARRAY_SIZE`, `ALIGN_NEXT`, `FOURCC`, ...) and the
Metrowerks intrinsics upstream declares for other compilers (`__memcpy`, `__cntlzw`, `__frsqrte`,
`__abs`, with bodies in `source/compat/Math.cpp`) took it to 1,355 (84%).

| Blocker | Files | Fix |
| --- | --- | --- |
| `NO_INLINE` after a definition's declarator | 359 on the first pass | empty, as upstream's own `types.h` has it for non-Metrowerks compilers |
| nw4r `math/arithmetic.h` inline PowerPC `__asm` (`FAbs`, `FSelect`) | 364 | generated copy with plain C++ bodies |
| nw4r `ut/LinkList.h` `friend class Self;` naming a typedef | 369 behind the asm | `friend Self;` |
| `Mario.hpp`: an anonymous struct of `TVec3f` inside a union (`_8B0`/`_8BC` over `mRelativeWallPos`) | 86 | the array alone; two sources use the old names |
| `operator new(u32)` where GCC wants `size_t` (`unsigned int` on ARM) | 46 | `size_t` in `JASHeapCtrl.hpp` |
| Metrowerks' `<mem.h>` | a few | `compat/mem.h` includes `<cstring>` |

What remains is per file: names declared later or elsewhere under Metrowerks' lookup (~140),
`ATTR_WEAK` on existing definitions (47), attributes on definitions outside `NO_INLINE` (43), and
the audio DSP headers Petari does not have.

**Why LiveActor is the gateway.** `LiveActor::movement` updates the model manager and animation,
calculates gravity, runs sensors, the nerve, `control()`, then `updateBinder`, which moves the actor
along its velocity against collision through `Binder` → `Collision::checkStrikeBallToMap` →
`CollisionDirector` → `CollisionCategorizedKeeper` → `CollisionParts` → `KCollisionServer` (the last
already runs natively). `CollisionParts` reads its `HitSensor`'s host actor, so the original collision
stack, the one Mario's movement uses, needs `LiveActor` and `HitSensor` first. `smg::StageCollision`
stands in until then.

**Milestones**, each on screen in the preview before the next:

1. **An original LiveActor falls and lands (done in the preview).** `LiveActor`, `LiveActorFlag`, `HitSensor`,
   `HitSensorKeeper`, `HitSensorInfo`, `Binder`, `HitInfo`, `CollisionDirector`,
   `CollisionCategorizedKeeper`, `CollisionParts`, `LiveActorGroup`, `AllLiveActorGroup`,
   `SensorHitChecker`, the clipping family (`ClippingDirector`, `ClippingJudge`, `ClippingActorHolder`,
   `ClippingActorInfo`, `ClippingGroupHolder`, `ViewGroupCtrl`) and the SDK's `GDBase` compile as they
   are. `LiveActor.cpp` is a generated copy without `initSound` and `addToSoundObjHolder`, so a caller
   fails to link; the `MR::` helpers they call are extracted from `LiveActorUtil`, `ActorSensorUtil`,
   `ActorMovementUtil`, `MapUtil`, `ObjUtil` and `ShadowController`. What reaches systems not brought
   up (models, animation keepers, effects, shadows, the star pointer, `ActorLightCtrl`, LOD, the
   player's position) stops the program by name (`source/compat/Actor.cpp`); the app sends stderr to
   `svcOutputDebugString`, which Azahar logs with the filter `Debug.Emulated:Debug`. `smg::CollisionHost` is a
   modelless `LiveActor` holding a placed part's `CollisionParts` from its own KCL and PA, as
   `initActorCollisionParts` builds it, with the files handed over instead of looked up in a
   `ResourceHolder`. `smg::ProbeActor` is a modelless `LiveActor` with a `Binder` and gravity, moved by
   `LiveActor::movement` from the scene's list: it lands on a KCL floor in 20 frames under a parallel
   gravity (host and ARM). Pressed down a unit each frame, the original Binder sets the sphere back
   about a unit above touching, so contact comes every other frame. On the host, `LiveActor.cpp`
   builds without UBSan's vptr check, which needs the type info of its unbuilt model and sound
   classes; the 3DS build has no RTTI. The preview gives every placed part a `CollisionHost` as its
   collision arrives, ends actor placement when the last one has, and drops the probe as a
   `ProbeActor`; the scene's movement waits for placement to end, as the game's does. `ViewGroupCtrlCube`
   areas are the original plain `AreaObj` cubes in an `AreaObjMgr`; `ClippingDirector` stays out of the
   movement until the player runs (view groups follow the player's position), so every actor stays
   unclipped, as each starts. `resource_audit --actors` drops a probe above every start on the disc:
   124 dropped, 116 land through their Binder, all on the part the ray audit finds (the other 8 have no
   collision below). Most rest within 5 units of the ray's landing; with no friction of its own the
   probe slides on sloped starts, up to 246 units on StarDust's.
2. **ModelManager and XanimePlayer.** All 31 of JSystem's J3D sources build as `petari_j3d` on the host
   and ARM. Its paired-single assembly keeps its Metrowerks branch and gains a plain C++ one:
   `J3DPSCalcInverseTranspose` (the rows are cross products over the determinant; a zero determinant
   leaves the destination alone, as the assembly returns early), `J3DScaleNrmMtx`/`33`, `J3DMtxProjConcat`
   (a 3x4 times a 4x4), `J3DPSMtxArrayConcat` (`PSMTXConcat` per matrix) and
   `J3DMtxBuffer::calcWeightEnvelopeMtx` (each envelope is its joints' world times inverse bind, weighted),
   each read from the assembly's register flow and checked against the direct formula (host and ARM).
   J3D's CPU side (`J3DModel::calc`, joints, envelope matrices) is portable; `J3DModelData` gets a native loader from the port's parsers instead of J3D's in-place
   big-endian loader. `XanimePlayer`/`XanimeCore` blend Mario's animations; the renderer draws what
   the original matrices say.
3. **MarioActor headless.** `MarioActor`, `Mario` and its state modules on the actor core, input
   through the original `WPad` path from `InputFrame`, camera direction through an explicit
   boundary. Mario idles, walks and jumps on Good Egg's first planet.
4. **The game camera.** `CameraDirector` and the camera classes (120 of 127 files already compile).
5. **Map objects.** `MapObjActor` and the factory for the common objects, so moving parts and
   switches act as in the game.
6. **Effects and sound**, deferred until the frame fits.

## The native scene boundary

`smg::NativeScene` (`compat/smg/Scene.hpp`, `source/compat/Scene.cpp`) holds what
`GameSystemSceneController` owns in the game, for the objects the port has brought up:

- the scene's `NameObjHolder` (same capacity, 0x1300), set as `NameObjRegister`'s current holder,
  so every `NameObj` registers itself as it does in the game;
- the movement-sync request. `MR::notifyRequestNameObjMovementOnOff` asks for it, and
  `NativeScene::movement()` answers it the way the first half of
  `SceneNameObjMovementController::movement()` does. The stop-scene states in its second half
  (demos, pause categories) are not brought up;
- the scene's `SceneObjHolder`, answering `MR::getSceneObjHolder` and `MR::isExistSceneObj`.
  The class and its own methods (`create`, `getObj`, `isExist`, `MR::createSceneObj`) are the
  original's, copied out verbatim; `newEachObj` makes `PlanetGravityManager` (as the game
  names it, "重力"), which the scene creates first, as `GameScene::init` does, then
  `SceneFunction::initForLiveActor`'s `AllLiveActorGroup`, `ClippingDirector`, `SensorHitChecker` and
  `CollisionDirector`, and the `StageSwitchContainer`. Any other scene object stops the program with a
  message naming its id.
- `NameObjExecuteHolder`'s registration (`MR::registerNameObjToExecuteHolder`) and its connect and
  disconnect requests for movement and draw. `movement()` runs connected objects' `executeMovement`,
  then their `calcAnim`, in registration order; the game's `NameObjListExecutor` runs them by category
  and applies requests on its next pass. `MR::connectToScene` itself is the original.
- `CameraDirector`'s view for clipping (`MR::getCameraViewMtx`, `getFovy`, `getAspect`, `getFarZ`),
  set by whoever draws, and `MR::getCurrentPlacementZoneId`. `endActorPlacement` is `GameScene::init`'s
  `MR::endInitLiveActorSystemInfo`, called once the last placed actor exists.

- the zone placements `StageDataHolder` keeps. `MR::isPlacementLocalStage` and
  `MR::getZonePlacementMtx`, which `JMapUtil` uses to move rows into the galaxy, find a row's
  zone by the table it came from, as the original finds it by address range. Each zone's matrix
  comes from the original `MR::makeMtxTR`, as `StageDataHolder::calcPlacementMtx` builds it;
- `GlobalGravityObj`'s work, until `LiveActor` runs (`source/compat/GlobalGravity.cpp`): pick the
  creator by the names `NameObjFactory` uses, call `createFromJMap` (which registers the gravity),
  read the row's switches into the original `StageSwitchCtrl`, appear, and put `SW_SLEEP` rows under
  an original `SleepController` whose listener does what the actor's `makeActorAppeared` and
  `makeActorDead` do to its gravity. `syncAfterPlacement` is GameScene's `initSyncSleepController`,
  so a gravity whose sleep switch is off starts asleep. `movement()` runs the sleep controllers, then
  `updateSwitch` for every awake gravity with switches: switch A must be on and B off. Switches live
  in the original `StageSwitchContainer`, one set of 128 per zone plus the global ones. A row linked
  to a map part (`MR::isValidFollowID`) keeps its placement: base matrix followers need the part's actor. Creators, gravities, switch
  controls, sleep controllers and the wire creator's `RailRider` are freed with the scene.
- `MR::getPlacedZoneId`, `MR::getZoneNum` and `MR::getZoneNameFromZoneId`, answered from the zones
  and the tables registered for them.
- `AreaObjContainer`'s light area manager (`MR::getAreaObjManager("LightArea")`), and no view cube
  manager for a stage that places no `ViewGroupCtrlCube` (the same to `ViewGroupCtrl` as an empty one;
  any other manager stops the program), `SwitchWatcherHolder`'s watch on switch-appear switches (validate and invalidate
  as the switch changes), and `SleepControllerHolder`'s control of areas. Areas linked to a map part
  are left out and listed, since they stand relative to the part's moving matrix.

- the original `CollisionDirector`'s `CollisionCode`, which `KCollisionServer` reads camera ids
  through, and `CameraDirector`'s camera-code collection:
  `MR::initCameraCodeCollection`, `registerCameraCode` and `termCameraCodeCollection` record, per
  part, the codes its collision names, for the camera to read when it runs.

Everything else about the scene still belongs to `GameSystem`. Asking for the scene before one
exists stops the program with a message. Once `GameSystem` and the original `SceneObjHolder.cpp`
run, delete the native scene, its `newEachObj`, and the holder lookups it answers.

## Pointer details to preserve

`StarPieceShooter::tryShoot` consumes both Wii B hold and trigger states, so ZR (and the
stylus touch-to-shoot toggle) produce B's trigger on the first pointed sample and hold
while it continues; the original game keeps ownership of shot timing and charge. B is
only reported while the pointer is valid, as with the Wii's IR. The toggle tap itself
never emits B. The camera reset maps to Nunchuk C, jump aliases to Wii A, crouch to
Nunchuk Z, spin to a digital shake, and the D-pad keeps its four original bits. Pointer
recentering is native and never reaches the game.

Aliased physical buttons are combined before transition detection, so pressing B while
holding A does not create a second jump edge, and releasing one does not release the other.

The motion pointer reads the gyro in the HID ring's order - pitch, yaw, roll, as Azahar
lays them out; libctru's `angularRate` names the same fields x, z, y. Which way each axis
turns on hardware, and the accelerometer's sign, still need checking on a console; the
tracker already accepts either gravity sign, and Options can invert both axes.

## Required validation

- Motion pointer direction, speed and drift at several holding angles on a New 3DS and a
  New 2DS XL; C-Stick range and deadzone on hardware.
- Exact top-screen corner mapping, lift-to-hide, both button aliases per action, and diagonal
  Circle Pad magnitude. Repeat these on hardware because touch calibration is device-specific.
- Toggle taps, long holds, drags onto/off either button, half-opacity feedback, camera in shooting
  mode, resume during a touch, and a new aim after resume.
- Real original icon provenance and visual readability at 3DS resolution.
- Original Mario movement and planet transitions before calling any build a gameplay port.
- Boot and frame-time evidence on hardware before making performance claims.

## Current USA disc validation

The RMGE01 disc provides the original purple star-bit, camera, and mirrored half-star
pointer textures. They load on emulated ARM through the external TPL-to-T3X pipeline.
The actual Good Egg scenario archive is `StageData/EggStarGalaxy/EggStarGalaxyScenario.arc`;
its six scenario records and first scenario number 1 have been read through original JMapInfo
on emulated ARM. A boot probe is not stage execution.

An ASan/UBSan host audit indexed 2,228 RARC archives with 24,788 resources and converted
all 1,405 files with the `.bcsv` extension (10,891 records). Extensionless stage tables
are not covered by that audit. Forty-nine animation tables required
repacking unaligned fields; regression tests cover packed offsets and odd record strides.
The four remaining `.arc` files are U8-format Wii HOME Menu resources, currently unsupported.
`resource_audit` reports them and returns failure rather than counting them as validated.

`resource_audit --gravity` places the gravity of all 136 scenarios in the 48 galaxies through
the original creators under ASan and UBSan, and asks every placed gravity for its pull at 48
points around Mario's start (181,392 pulls, none infinite or NaN). The disc has 1,075 global
gravity rows, all in `planetobjinfo` tables: 463 planes in boxes, 178 points, 147 cubes, 140
disks, 75 segments, 34 planes in cylinders, 10 disk tori, 9 planes, 7 cones and 12 wires. Every
kind is placed: the 59 wire placements (22 scenarios, among them Honeybee Kingdom, Sand Clock and
Cosmos Garden) sample their real rails into 1,095 points, each from its rail's first point, with
chords within 5% of even (the original integrates Bezier lengths numerically, a little short).
294 placements follow their switches; 61 start asleep because their `SW_SLEEP` switch is off.
2,175 light areas are placed across all scenarios (9 linked to map parts wait). 63 rows use switch A, 15 switch B, 11 `SW_SLEEP`, and 78 are linked
to a map part (`MapParts_ID`, `Obj_ID` or `ChildObjId`, which `MR::isValidFollowID` reads through
`JMapLinkInfo`): those follow the part's moving matrix in the game and keep their placement here. Every scenario has gravity at Mario's start except twelve hidden-star scenarios
(`IsHidden`), which place no MarioNo 0 entrance of their own; `Stage::entrance` says so instead
of starting at the origin. Another 67 rows place actors with gravity of their own (the reverse
and quarter-roll gravity rooms, gravity lights); they wait for `LiveActor`. The audit then follows
each start's pull onto the placed models' collision, from 100 units back along it (a start may sit
on or just under the surface): 116 of the 124 scenarios with an entrance land, 69 within 50 units
below, 47 on a surface up to 50 units above the start point (most within 10). The eight that meet
none start in the observatory dome, whose parts choose their collision by story state, or on a ship.

`resource_audit --collision` repacks all 1,016 KCL files on the disc (677,800 triangles, 424 left out
by the game as degenerate), sets each up as `CollisionParts::init` does, and casts 48 rays through each
with `checkArrow`, checking every result against brute force over all triangles. 27,856 hits agree; the
rest differ only where the original does: 10 at an edge (`KCHitArrow` accepts 0.01 outside it), 240
where the disc's octree does not list the nearer triangle in the block the ray meets it, and 48 where
the walk stops at the first block with any hit although that hit lies beyond it. None is unexplained.
One file has no attribute table (`CocoTree`) and one triangle of `BigBigFly` names a missing row; the
game ignores both.

All 1,758 BMD/BDL models load under ASan and UBSan (5,053,844 triangles), and 6,987 of their
7,780 materials translate to the combiner exactly. Good Egg scenario 1 places 296 objects
across 12 zones; the preview draws 56 rigid models in 172 placements.

The portable math tests check tolerances and conventions, not bit-identical Gekko rounding.
The native validation app passes 1,069 math, 37 WPad, 179 BCSV, 1,043 J3D model/texture,
8,810 TEV combiner, 27 original gravity, 27 NameObj, 89 scene and gravity creator, 48 collision, 21 J3D animation, 13 original actor, 6 J3D transform, 7 ETC1 and 8 GPU texture checks on emulated ARM
in Azahar; the GPU checks compare the hardware's ETC1 decoding against ours. Of the disc's
7,780 materials, 6,987 translate to the combiner exactly. No physical console has been tested.
