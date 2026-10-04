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
  The game's `KPADRead` uses the same input: `smg::HidMotion` (shared with the native app)
  switches the gyroscope and accelerometer on in `KPADInit` and feeds them to the pointer.
  A frame longer than a quarter second moves the pointer by a quarter second's worth: the
  rates are one moment's, and dropping such frames left the pointer frozen at low frame rates.
  The game's `WPadPointer` waits out IR noise (5 valid samples before showing, 10 invalid ones before hiding);
  the 3DS pointer is exact, so both are 0 and a lifted stylus hides it at once. Shoot (R) stays the Wii's B
  with the stylus lifted: the title's "Press both A and B" and the menus' back need it.
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
  the look changes little until actors draw. Petari's sources are UTF-8 and the disc's names Shift-JIS; the
  build compiles the game's string literals as Shift-JIS (CP932, `-fexec-charset`), as Metrowerks did, so
  `LightDataHolder`'s "デフォルト" matches its row by name (the first row, its old fallback, anyway). Skinned placed models are still skipped: which
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
   `smg::J3DSkeletonImage` gives JSystem's own `J3DModelLoader` a model's skeleton to read in place: INF1
   (its hierarchy keeping only joint nodes), JNT1, EVP1 and DRW1, native-endian and laid out in the
   loader's structs (they keep offsets in pointer-sized fields, so a byte copy would only suit 32-bit
   targets). As on the disc, DRW1 ends with its envelope entries again, which the loader counts off.
   `J3DModel::calcAnmMtx` and `J3DMtxBuffer::calcWeightEnvelopeMtx` then run as the game runs them, on the
   host and ARM. J3D reaches the Wii GPU's API from its drawing side; each GX entry point stops the program
   by name (`source/compat/Gx.cpp`), and the SDK's write-gather writers go through one point that does the
   same. Cache stores and interrupt or scheduler sections do nothing, which is their whole purpose here: no
   reader outside the CPU sees that memory, and J3D runs on one thread. J3D's 32-byte alignment requests
   (the GPU's DMA granularity) take the free store's alignment. JSystem frees nothing in its destructors;
   `smg::forgetJ3DSkeleton` frees a skeleton model as the scene heap going would.
   Checking every model on the disc against it showed the port's own joint composition was wrong for
   294 of 1,758 models, Mario's among them (8% off): J3D composes joints three ways by INF1's flags. Basic
   applies a joint's scale only while the accumulated scale is not one; Softimage scales translations by
   the accumulated scale and applies the scale at the end; Maya divides a scale-compensating joint by its
   parent's own scale. It also turns by its sine table. The port now composes the same way from J3D's own
   `J3DGetTranslateRotateMtx` and `JMAMTXApplyScale`, and `resource_audit --models` finds all 1,758
   skeletons agree with `J3DModel` to 1e-4. (The earlier bind-pose check could not see it: the port's
   loader baked its vertices with the same composition it was checked against.)
   Animated actors are posed by the game's own `XanimeCore`, a `J3DMtxCalc` of blended BCK tracks, which
   builds on the model's `J3DModel` (`petari_anim`). It turns joints through quaternions made from
   half-angle sine lookups, so over the disc's 2,769 BCKs that fit a model of their archive it sits a
   little apart from J3D's own Euler calc (median 0.009 of a unit, worst 3.8 at the end of BigBigFly's long
   chain); the preview now poses Mario with `XanimeCore` itself, as `XanimePlayer` sets it up for one BCK,
   and only skins and draws what `J3DModel` computes. J3D keeps its state in statics, so it runs on the
   main thread only; the loader thread uses the port's own composition, which agrees with `J3DModel`.
   J3D's CPU side (`J3DModel::calc`, joints, envelope matrices) is portable; `J3DModelData` gets a native loader from the port's parsers instead of J3D's in-place
   big-endian loader. `XanimePlayer`/`XanimeCore` blend Mario's animations; the renderer draws what
   the original matrices say.
3. **MarioActor headless.** `MarioActor`, `Mario` and its state modules on the actor core, input
   through the original `WPad` path from `InputFrame`, camera direction through an explicit
   boundary. Mario idles, walks and jumps on Good Egg's first planet.
   Measured with `tools/petari-link-needs.sh BUILD Game/Player OUT`: all 107 player sources compile for
   ARM (after `Inline.hpp`'s `NO_INLINE`, a header named in another case, the SDK's angle macros,
   comparisons of a `bool` or a pointer-holding `u32` with `nullptr`, `Mario.hpp`'s union names, and
   case labels that jump past declarations), and linking them needs 543 symbols the port does not yet
   define: 353 `MR::` helpers (by file, LiveActorUtil 46, SoundUtil 25, CameraUtil 24, MapUtil 22,
   PlayerUtil 22, EventUtil 20, GamePadUtil 19, AreaObjUtil 16, ActorSensorUtil 15, JointUtil 13,
   ModelUtil 12, ScreenUtil 12, ActorShadowUtil 10, and a tail), 53 SDK functions, `XanimePlayer` (28),
   `TDDraw` debug drawing (16), effects (`MultiEmitter`, `EffectKeeper`), `PartsModel`, layouts, and
   single references into bosses, map objects and races that Mario's reactions name.
4. **The game camera.** `CameraDirector` and the camera classes (120 of 127 files already compile).
5. **Map objects.** `MapObjActor` and the factory for the common objects, so moving parts and
   switches act as in the game.
6. **Effects and sound**, deferred until the frame fits.

## Running the game's own boot

Since session 3's goal 19, every Petari source compiles for the 3DS: the 1,605 game files, JSystem's 169 and
nw4r's 29. `petari_game` (CMakeLists.txt, 3DS only) builds all of them that the smaller libraries do not, and
`mario_probe.3dsx` links it with `--gc-sections`, so a program keeps only what it reaches. The probe calls the
game's own `main()` (GameSystem.cpp, kept whole as `smgGameMain`) and runs until the first thing the port
lacks; every such stop prints its name to stderr, which the probe routes to Azahar's log (filter
`*:Info Debug.Emulated:Debug`, and **`instant_debug_log=true`**, or the log file stops a second in). A
priority-101 constructor opens that channel before the game's static initializers run. Every stop ends in `abort`,
and the probe ends the process right there: `abort` is wrapped at link time (`--wrap=abort`, since newlib keeps signal
handlers per thread and most stops happen off the main thread), because libctru's own exit would unmount the SD card
while the game's DVD worker was still reading from it, which faults on hardware. Before ending, it prints the
return addresses left on the stopping thread's stack ("callers on the stack": each word that follows a call
instruction in the program's code); `tools/stop-callers.sh` names them from the ELF, newest first.

**Measuring.** `tools/petari-link-needs.sh BUILD "Game JSystem nw4r" OUT HEADER 'STATEMENT'` compiles every
source for ARM, lists what nothing defines, and links a probe of `STATEMENT` against the port's libraries with
section GC: `probe-undefined.txt` is what that code really reaches. `tools/petari-closure.py OUT OBJECT` gives
the object-level closure. For MarioActor: 1,652 objects, 205 SDK symbols undefined, 83 defined twice.

**Who owns a function.** Where the port's native scene, pad or allocator and the originals both define
something, `NATIVE` in `tools/prepare-petari.py` names the owner and leaves it out of the original's copy
with the reason. For each file the scene libraries only take extracts from, the prepare script writes the
rest of it at the source's own path (inline and template helpers stay; `rest_fixes` carry shared fixes), so
the two libraries never define a function twice. `petari_game_boundaries` holds the loud stops for game
classes that `petari_game` builds for real (ModelManager, EffectKeeper, ...); it links after everything,
so only programs without the game (the preview, tests, tools) ever take one.

**The SDK layer** (all in `source/compat`, 3DS only unless noted):

| Library | File | What it is on the 3DS |
| --- | --- | --- |
| OS threads, mutexes, messages, clock | `OsThreads.cpp` | libctru threads on the app core, Wii priorities mapped in order (16 = 0x30); recursive mutexes on LightLocks; blocking queues; time base at 60.75 MHz from the system tick, dated from 2000-01-01 |
| Arenas | `OsMemory.cpp` | MEM1 (0x806C0000-0x81800000, from main.dol's end) and MEM2 (0x90000800-0x935E0000, the arena IOS33, the disc's IOS, leaves) as two blocks of the 3DS heap; boot info with the Wii's 24 MB |
| Allocation | `GameHeap.cpp` | plain `new` from the game's current JKRHeap once the root heap exists (as the original), the free store before; `delete` by address. The port's own long-lived containers use `compat/smg/FreeStore.hpp`'s allocator, and port code the game calls into opens an `smg::PortMemory` scope, so its scratch never spends a game heap (a solid heap would never get it back) |
| Disc | `Dvd.cpp`, `DiscImage.cpp` | entry numbers from the disc's file table, the RVZ image's or `sys/fst.bin` (not reproducible by sorting: MovieData precedes ModuleData); reads from the image or the extracted files; async reads finish on a worker that calls back like the drive's interrupt |
| Video | `Video.cpp` | a retrace thread on the 3DS VBlank runs the game's retrace callbacks, latches flushed buffers |
| System settings | `SystemConfig.cpp` | language and sound mode from the 3DS's settings; 16:9 for the 5:3 screen |
| WiiConnect24 | `Nwc24.cpp` | answers as a Wii with it switched off; the game sends no letters |
| ARAM | `RVL_SDK/aralt/aralt.c` | the SDK's own "Dummy ARAM" in MEM2, compiled as it is |
| Pure software | `Os.cpp`, `Math.cpp` (host too) | OSReport, OSPanic, PPCHalt, cache ops, MSR, error handlers kept; the SDK's matrix helpers |
| GPU (GX, GD) | `RVL_SDK/gx/*.c`, `gd/*.c`, `GxFifo.cpp` | the SDK's own GX and GD, built as C++ with C's conversions; its register blocks are port memory (hardware blocks the port has no counterpart for stop by name); the FIFO takes big-endian bytes into a command processor (`smg::GxCommandProcessor`, portable and tested) that keeps CP/XF/BP state, follows display lists, raises the pixel engine's draw-done and token interrupts, sizes and decodes vertices by the CP's descriptor and attribute tables (the field layout of the SDK's own `private/cp_reg.h`), and hands each whole primitive to the host, which stops there until drawing is written |
| GPU drawing | `GxRender.cpp`, `smg::readGxDrawState`, `smg::GxVertexPipeline` | the draw state (registers through the SDK's own `private/*_reg.h`, TEV to combiner stages) is read again only when a register a draw reads changes; each primitive's vertices are transformed and culled on the CPU, and consecutive primitives under one state are one indexed citro3d draw on the top screen; the display copy ends the frame |
| Audio | `WiiAudio.cpp`, `smg::WiiDsp` | the Wii DSP running JAudio2's own microcode protocol in software (ROM boot, task mails, commands, voice mixing, filters, reverb); the AI's DMA blocks play through the 3DS's DSP (NDSP), raising the AI interrupt per block |
| Alarms, thread priority | `OsThreads.cpp` | the SDK's alarm list on one alarm thread (one-shot and periodic, as `InsertAlarm` orders them); priorities read and change |
| Reset, power, HOME | `OsReset.cpp` | the power callback runs once when the 3DS asks the app to close; no reset button |
| Remote extras | `Wpad.cpp` | battery from the 3DS's own, charging as full; the Remote's speaker and motor answer that there is none |
| Interrupts | `Os.cpp` (host too) | the SDK's handler table and mask; a raised interrupt waits while interrupts are off and is delivered when they come back on, lowest source first, its handler run with interrupts off (GXSetDrawDone depends on exactly this) |
| Everything else reached | `Sdk.cpp` | generated by `tools/sdk-boundaries.py`: one loud stop per C function |

**Paired singles only Metrowerks compiles.** Some JSystem functions keep their whole body in
`#ifdef __MWERKS__` asm with no `#else`; GCC compiles them empty or reading registers never set.
`tools/mwerks-only-asm.py PETARI BUILD/petari Game JSystem nw4r RVL_SDK/gx RVL_SDK/gd RVL_SDK/aralt` lists them,
and empty macro fallbacks, in everything the port builds, as the build resolves each header: it found 13 asm
blocks, eight of them real bugs in waiting (`J3DCalcZValue`, `JMAVECLerp`, `JMAVECScaleAdd`, `JMAQuatLerp`, `J3DTextureSRTInfo::operator=`,
`J3DMtxBuffer::setNrmMtx`, `J3DTexMtxInfo::setEffectMtx`, two 3x3 copies). All have C++ bodies now; the scan
reads 0. Run it after any new file joins the build.

**Endianness.** The Wii's CPU is big-endian and reads its files with plain loads; the 3DS's is little-endian.
Every original parser meets this, format by format. `source/compat/NativeResources.cpp` turns a format's
structures native in place where original code first reads them, recognising data it already turned. RARC
archives are first (at `JKRMemArchive`'s mount); `JKRArchive::SDIFileEntry`'s `mFlag:8 / mNameOffset:24`
also swaps declaration order, since Metrowerks fills bitfields from the top and GCC on ARM from the bottom.
BCSV tables turn at `JMapInfo::attach` (in place, or as a repacked copy where ARM can't load a field where it
sits). A table repacked once marks its original's header with its copy's address, so every later attach
takes the same copy, as the Wii's attached the table itself; before that each attach made a new copy, and the Star
Festival's Toads, each attaching their animation table, filled a 20 MB scene heap. Since session 4, `smg::SwapPlan` (`include/smg/SwapPlan.hpp`) is the shared tool for the rest: a converter
walks a file exactly as its reader does and plans every field once (kept bytes, half or word); bytes two readers
would take differently, fields past the end, or words ARM can't load where they sit fail the plan, and the file stays
as it was with the reason. Layouts (BRLYT, BRLAN, TPL, BRFNT: `source/core/Layout.cpp`, turned where
`LayoutHolder` mounts them and `ResFont::SetResource` takes a font) and the audio data (`source/core/AudioData.cpp`:
the audio archive SMR.szs with its sound table, names, sequence collection, wave systems and instrument banks; the
chord, music-event, remix and Remote-speaker files) are done, each with a disc audit that reads the result back
through the original's own structs: `resource_audit FILES --layouts` (894 files, 232,439 fields, none different)
and `--audio` (79 files, 69,003 fields, none different). What stays big-endian stays so on purpose: texels and
glyph sheets (the GPU's bytes), sequence bytecode (JAudio2's reader reads bytes, adapted), wave samples (the DSP reads
them big-endian, as the Wii's did), and the sequence collection's group tables, which sit at odd addresses.
Still to come as the boot reaches them: J3D models and animations, collision (KCL), particles, streams (AST).

**Messages (BMG).** The game reads its text more than one way, so no single byte order serves it. Characters
and a tag's type are 16-bit units; `MessageEditorMessageTag` reads a tag's length and group as bytes, and so
does `TalkNodeCtrl`. Parameters are read as bytes, halves, words or text, depending on the kind. `smg::nativeBmg`
(`source/core/Bmg.cpp`) walks each message from its INF1 offset and plans every unit once: characters and
types are swapped; a tag's (length, group) unit keeps its bytes; parameters follow the game's reader for their
kind. Those are: a wait's `getParam16` (group 1 type 0), a sound or font name's `getParamPtr` text (groups 2
and 10), a player name's or colour's `getParam8` (group 5, group 255 type 0), and a number or string tag's
slot plus `getParam32(1)` argument (groups 6 and 7). The game writes the slot natively before it reads it, and
it is empty on the disc. FLW1's message and branch nodes read two halves after the index, event nodes one word.
FLI1 is never read, and on the disc its declared size runs past the file, so only its block header turns. A
kind with parameters the port has no layout for is refused whole, by name: the system group's ruby (its text
sits at an odd byte), group 8's message index and group 11 aren't on the US disc. `resource_audit FILES --bmg`
turns every BMG on the disc and reads each field the game's way against the original: 3 files, 7,392
messages, 306,992 characters and tags, 3,948 flow nodes, 50 tag kinds, none refused, none different.
`MessageData` calls it (`smgNativeBmg`) right after taking the file from its archive.

**Pads.** The whole-game boot builds the game's own `WPad` and `WPadHolder` with all their parts (button,
pointer, acceleration, swings, rumble, info checker, leave watcher) over the port's KPAD
(`source/compat/Kpad.cpp`). Channel 0 is the 3DS itself, read through the port's own input mapping
(`smg::readHidInput`, `smg::Input`, `smg::toWiiInput`, `smg::writeKpadStatus`), and the other three channels
never connect. The WPAD side (`source/compat/Wpad.cpp`) reports channel 0's connection and its Nunchuk at the
first read, as the Wii's Bluetooth stack reports them some time after the callbacks are set, never inside the
call (the game's callback looks its pad up through a `WPadHolder` still being built). The sensor bar answers
below the TV, the SDK's own answer when its setting is missing. Motion isn't fed to KPAD yet. The native scene's
pad (`source/compat/NativePad.cpp`, library `smg_native_pad`) owns its `WPad` the native way and is linked only
by the native app and the tests, so the boot never meets it.

**Where the boot stands** (2026-09-30, 10:55): through static initialization, DVD, VI, the root heap and
all of `HeapMemoryWatcher`'s heaps, nw4r's layout init, JKRAram's MEM2 ARAM, the error archive's mount, the
language setting and WiiConnect24, the SDK's own `GXInit` (every setup command it sends decoded), the layout
holders over the error message archive, `MessageData` over the system messages, and the game's own pads, to its
first layout. `LayoutManager::removeUnnecessaryPanes` walks a pane tree nw4r built from big-endian BRLYT data,
so layouts (BRLYT, then BRLAN) are the next format to turn native. The text itself is not drawn yet.
That needs `wchar_t` settled (below): the game reads its UTF-16 text through `wchar_t*`. Rendering is the other
open design: the command processor has the state, and turning primitives into citro3d draws is next, with the
port's TEV-to-PICA combiner and texture conversion.

**Colours as words.** nw4r's `ut::Color` and JSystem's `JUtility::TColor` turned to and from `u32` through
memory, which on the Wii's big-endian CPU packs `0xRRGGBBAA`. On ARM the same code reverses the bytes, so a
literal like `0x000000FF` would have been transparent red instead of opaque black, and J3D's material and
ambient colours (`loadMatColors`, `loadAmbColors`, `J3DGDWrite_u32(*(u32*)color)`) would have gone to the GPU as
ABGR. All three now pack and unpack the value the Wii's way (R in the top byte). The same pattern built every TEV
stage's two BP commands (`J3DTevStage::load`, `*(u32*)&field_0x0`: the register byte, then three setting bytes),
which on ARM would have named a setting as the register; they are packed by value too. `tests/ColorTests.cpp`
pins both, the TEV stage through GD into the port's command processor. `J3DShapeDraw` read each primitive's
vertex count out of a display list as a native `u16`; it is the GPU's big-endian half, so a strip of 3 would
have counted 768 and the walk (`countVertex`, `addTexMtxIndexInDL`, which `ModelUtil` calls) run past the list.
It reads and writes those halves big-endian now. Plain copies between same-typed structs through `u32` keep
their bytes on either CPU and are left alone. Resource colours the game reads as `u32` (BRLYT's `vtxCols` and
`textCols`) must therefore be word-swapped when their format is turned native, like any other word.

**Bitfields read as words.** Metrowerks packs bitfields from each word's top bit; GCC on ARM from the bottom.
That only matters where code reaches a bitfield struct through the word it shares a union with. Of the 17 files
that declare bitfields, Mario's state unions are the ones used that way: `MarioJump` set two draw-state bits
with word constants (`0x00100000`, `0x2000`: fields `_B` and `mIsUnderwater`), and `MarioActor` tested bit 8 of
the high movement word (field `_37`). They name the fields now. Petari names these fields by their index in
hex, which makes the mapping checkable. Whole-word clears and copies are safe either way. Not yet run: the boot
doesn't reach Mario.

**Raw readers of file data.** `JKRDecomp::decodeSZS` took its end from the Yaz0 header's size as a native word;
it reads it big-endian (`JKRDecompExpandSize`) now. The boot's archive mounts decompress through another path,
so this one isn't exercised yet. Game, nw4r and JSystem code hold 191 such casts in all; most mark formats still
to turn native (THP movies, JPA particles, BMD models, camera animations, audio banks, save data).

**Upstream fallbacks that compile to nothing.** Besides the asm blocks, some SDK headers give non-Metrowerks
compilers empty definitions meant only for an editor's parser: `GXRegs.h`'s `FAST_FLAG_SET` (which packs every
GX register's fields) and its counter reads, `GXFDLShortcut.h`'s `FAST_GPFLAGSET`. Each now has a real body
(an `rlwimi`-style insert under its mask). Check any SDK header newly compiled for `#else` branches that
`#define` a function-like macro as nothing.

**Reads past an array's end.** Metrowerks compiled them as plain loads; gcc at -O2 treats them as undefined
and may drop the read or the loop around it. `tools/ub-scan.sh` recompiles every Petari object of a configured
3DS build with `-Wall` and reports gcc's `iteration N invokes undefined behavior`, `-Warray-bounds` and
`-Wstringop` findings, and any object that didn't compile. It found five, now adapted:
`CameraShaker::createSinglyHorizontalTask` runs 7 times over 3 tasks (Nintendo's own; the spill landed in
`mInfinityTasks`, replaced on the next line, so it stops at 3 and only four leaked allocations differ).
`StarPointerTransformHolder` copies a 4x4 projection from a 3x4 table, and Petari's symbol map shows the Wii read
`StarPointerPeekZ`'s vtable and a string for the last row, which a zero row stands in for.
`J3DTevBlock4::patchTexNoAndTexCoordScale` loops over 8 texture numbers where the block has 4 (J3DMatBlock.cpp is
NonMatching in Petari, and the class's own `patchTexNo` reads 4). `TVec3::setTrans` and `Swinger` read the
same floats through `mtx[row][col]`. The scan reads 0 over 1,872 objects; run it after large source changes.
It can't see every returned local: `SimpleEffectObj::getClippingCenterOffset` returns the address of a named local
(`TVec3f vec; return &vec;`), which gcc compiles to `return 0` without a warning, so the first galaxy with such an
effect (Gateway) crashed in `SimpleEffectObj::init`. A local static holds it now, as for the temporaries in
`MorphItemObjNeo.hpp` and `EffectObjGravityDust`; a search of the decomp for `return &name;` after a local
declaration finds no other.

**Which zone a placed object is in.** `StageDataHolder::findPlacedStageDataHolder` told an object's zone (whose rails,
start points and matrix it uses) by address: whether the object's row lies between the lowest of a holder's tables and
the end of its highest. That held on the Wii, where each zone's tables sat together in its archive. In the port the
spans can overlap (a galaxy's tables come from more than one archive, which load in the port's own order, and a
repacked table lives in a block of its own; which of the two did it wasn't pinned down): Gateway's rails were then looked up by ID in the galaxy's own path table, found nothing, and
`BezierRail` read the row past its end. The holder is now the one that has the row's table among its own.

**MEM2 as IOS33 leaves it.** Good Egg stopped while loading: its scene heap in MEM2 (`MR::getSceneHeapGDDR3`, the rest of
MEM2 after the 14 MB dummy ARAM, the resident heap, the HOME Button and WPAD heaps and the 16.25 MB file cache) had
1.9 MB, and its image effects' screen textures and water filter need 2.45 MB. The resident heap's contents match the
game's tables to the byte; the arena didn't: the disc boots IOS33 (its TMD's system version), whose MEM2 arena ends at
0x935E0000 (Dolphin's per-IOS table), and the port had the older IOSes' 0x933E0000. With the 2 MB back the scene heap has
4.0 MB and Good Egg plays.

**Japanese names.** Petari's sources are UTF-8, and the game's 5,081 non-ASCII string literals (demo, part, position,
joint and object names) compiled as UTF-8 bytes, while the disc spells the same names in Shift-JIS: every lookup by such
a name missed. Gateway's runaway Luma asks for its cutscene "チコガイドデモ" by name, found none, and crashed. The
Petari libraries now compile with `-finput-charset=UTF-8 -fexec-charset=CP932` (on `petari_core`, public), as
Metrowerks compiled them from Shift-JIS source; every one of the literals has a CP932 spelling.

**The C library.** MSL's printf writes nothing for a null `%s`; newlib writes `(null)`. The game builds archive
paths that way (`LayoutHolder`, `ResourceHolder`: four sites adapted). Other calls may lean on it; a general fix
would give game code MSL's own formatter (its decompiled `printf.c`) or wrap the printf family.

**Wide gaps, measured.** 64-bit hosts: 94 of the 1,803 files need fixes (`long` overload ambiguities, pointer
casts to `u32`), and the game stores pointers in `u32`, so a 32-bit host is the honest target for sanitizer runs.
`wchar_t` is two bytes on the Wii, and the game reads its UTF-16 text through `wchar_t*` (`TalkNodeCtrl`, the tag
processors, nw4r's `TextWriter`). The whole 3DS build uses `-fshort-wchar` (linked with `--no-wchar-size-warning`),
and the game's four wide functions (`swprintf`, `vswprintf`, `wcslen`, `wcsncpy`) are MSL's own from the decomp,
built as C over `compat/smg/MslWide.h`; `__num2dec` (a float in wide text) stops by name, as the game's wide formats
have none. MSL's `std::sort` orders equal keys differently from libstdc++'s.

**Traps met in session 4.** devkitARM's GCC makes enums one byte (ARM EABI short enums) where Metrowerks used
`-enum int`: any struct read from a file with an enum member changes shape (TPL's headers were; GX's
`GXVtxAttrFmtList`/`GXVtxDescList`, which J3D's model loader reads from BMD data, still are). Tests on Wii
addresses (`>= 0x80000000` for "cached") mean nothing here: the OS address macros are identities and four sites
were rewritten by meaning. GCC puts a class's vtable pointer first where Metrowerks put it after the first class's
data, so `JGadget::TLinkList` offsets written as numbers must be `offsetof`. The 3DS loads `.rodata` read-only on
hardware (Azahar doesn't enforce it), so data converted in place can't be `const`. And a new generated Petari
header needs both build directories cleaned: ninja's dependency files still name upstream's copy, and a stale object
kept a big-endian reader reading natively.

**One CPU, many threads.** The Wii runs every thread on one core, and "interrupts off" means nothing else runs. The
port makes it exactly that: `OSDisableInterrupts` takes a global lock (each thread keeps its own state, as each Wii
thread had its own MSR), interrupt handlers run holding it, and pending interrupts are atomic bits. A device raising
one only *tries* the lock, since it may be inside the GX FIFO, and whoever releases the lock looks again, so nothing is
lost between the two. The lock order is the CPU before the FIFO. Every OS wait (a message queue that would block, a
contended mutex, `OSSleepThread`, a thread's own suspension, sleeps, joins, the retrace wait) gives the CPU back
while it waits, as the Wii's scheduler loads the next thread's machine state. `OSSleepThread` notes its wake-up count
before letting go, and lets go without the sleep lock held, because a handler run at that moment may wake its own
queue. Alarms, retrace callbacks and disc callbacks run as interrupts. A spin that waits for an interrupt (the DSP's
boot handshake) sleeps a little each turn: on the 3DS the interrupt comes from other threads, which a spinner of
higher priority would starve. The GX FIFO assembles each thread's commands separately (four streams), and its lock
is fair: the main thread blocks only on citro3d's frame sync inside a draw, so with libctru's unfair locks the
scene-loading thread's GX writes never got a turn.

**Converted where archives mount.** `smgNativeRarc` turns each archive's tables native once, then its BTI headers,
KCL collision (in the Wii's own layout, whose offsets become 32-bit pointers; a 64-bit host keeps `Kcl`'s repacked
copy) and JParticle containers. JParticle's loader compares block tags as multi-character constants, which are
big-endian integers, so its tags turn too; before that, `'2-10'` never matched on ARM and every emitter was null.
Each converter has a disc audit that reads the result back through the game's own structs (`resource_audit --j3d`
covers J3D, BTI and JPC; `--collision` compares every KCL turned in place with the repacked one).

**The GX features the game scene needed.** Indexed XF loads (arrays 12 to 15: J3D's matrices and GX's light objects,
read in the CPU's order). Lighting for colour channel 0, after Dolphin's software renderer: ambient plus each masked
light, spot or specular falloff, diffuse none/signed/clamped, then the GPU's `(m * (l + (l >> 7))) >> 8`. EFB copies to
textures: `GxCopy` encodes every colour format but RG8 and GB8 as the pixel engine writes them, including the vertical
filter; the SDK's "no filter" setting is the taps (21, 22, 21)/64, since six-bit taps can't pass a row through
unchanged. The renderer reads its target back for each copy and for `GXPeekZ`/`GXPeekARGB` (the screen's way round:
EFB pixel to target index `sx * 240 + (239 - sy)`, checked against a live frame), quantised to the EFB's pixel
format; depth reads back as raw D24S8 words, with the port's nearer-is-larger turned to the Wii's Z. Indirect
texturing has no PICA counterpart (no dependent reads): a stage that only offsets its coordinates is drawn without
the offset, with a one-time note, and set-ups that wrap, replace or chain coordinates stop by name. A frame whose
commands outgrow citro3d's 1 MB buffer is ended and begun again with the screen output detached. GX's scissor
(BP 0x20/0x21, less the 342 offset) is scaled to the screen and set on the target the way citro3d's quarter turn
wants it: left = 240 - (y + h), top = 400 - (x + w).

**TEV comparisons.** The PICA's combiner has no compare ops (`GX_TEV_COMP_*`), which compute `d + (a > b ? c : 0)`
and friends. Image effects lean on them: `FallOutFieldDraw`'s screen fill sets alpha to `texture alpha > K0.a ? A0 :
0` and lets an alpha test drop the zeros. With A0 = 0 that pass is invisible on the Wii. Read as an ordinary lerp,
it painted its colour over the file select's planets. A compare whose c is zero is exactly d, so it compiles
exactly. A greater-than compare by component (RGB8, A8) or by red (R8: the bloom's bright pass, `TEXC.r > RASC.r ? 1 :
0`) is built from saturating steps: for eight-bit inputs `a > b` is `(a - b) * 256` saturated, which is a subtract
times four and then two doublings times four, followed by one op for `c * step + d`: four PICA stages at most, exact
on byte inputs (`compareAddsC` checks ties). The title's sea of light draws with one: alpha is `texture alpha >
K.a ? vertex alpha : 0`, and the game raises K as the file select arrives, so the sea dissolves away. Read as d, it
stayed on screen behind the planets. Equality needs both one-sided steps kept apart in the buffer, six ops for
one GX stage, and the 16- and 24-bit compares read across components: those add nothing and are counted as
approximations, so the effect is left out rather than spread over the screen. 16 of the disc's 7,780 materials still
need one (27 before), mostly the water's `R8_EQ` masks.

**EFB copies on the GPU.** Encoding a copy texel by texel was the port's largest cost: in the file-select level the
game copies the whole screen every frame (`CaptureScreenDirector`'s RGB565 capture, `ScreenAlphaCapture`'s A8 one for
`MR::fillSilhouetteColor`), and a sampling profile of the title put 55% of the 3DS's CPU in the encoder and in decoding
its result back for the PICA, at 1 to 4 frames a second. Every such copy goes through `JUTTexture::capture`, whose
image the game only ever loads as a texture, so that function (adapted) has the renderer keep its copies on the PICA:
the target is taken whole into a 256x512 texture by a raw texture copy (its rows of tiles regapped from 240 to 256
pixels), and a quad draws the copy's rectangle into a VRAM texture of the copy's own (about screen resolution), its
combiner summing up to three of the vertical filter's taps and turning the result into what the copy format's read
gives: intensity as the PE's studio-range Y (a DOT3 stage), one channel for every component, or colour with alpha one
for RGB565. A draw that binds the destination address gets that texture. The Wii's memory is not written, so the CPU
never sees these copies; every other copy (the Message Board photo, `captureDolTexture`) is encoded into memory as
before, which also forgets any GPU copy at its address. The first copy of each kind is read back and set against the
CPU's encoding of the same pixels (the log says by how much, and that a mirrored or upside-down reading would differ
more). Not reproduced: the EFB pixel formats' reduced precision, and filters with more than three taps (those copies
stay the CPU's). Inside a frame citro3d's "sync" transfers only queue, so `readEfb` (CPU copies, `GXPeekZ`/`ARGB`)
ends the frame without showing it, reads, and begins again: on hardware the old readback ran before the GPU had drawn.
The Star Festival's first frames then showed the per-primitive costs: `compileTev` ran for every primitive (46% of the
3DS's CPU after Play This File), so `buildGxDraw` keeps each program by the fields it reads (stage count and stages,
TEV and konst colours, swap tables, each stage's texture), and the ETC1 encoder for CMPR textures fits one split (the one
whose halves' averages fit the block better) in one mode (differential when the halves allow) instead of searching all
four.

**Drawing in batches.** The renderer's own cost was the next wall: in the Star Festival's prologue a frame took 1.1 to
3.4 seconds in Azahar, 88 to 95% of it turning GX primitives into 3DS draws. Up to 30,000 primitives a frame (small
quads, at four or five vertices each) each paid for a whole draw: the TEV program and pixel tests read again, structs reset,
citro3d's state set and a draw issued, about 29,000 instructions apiece. Yet 85 to 99.6% of them came with no register
changed since the one before. So the command processor counts changes by kind (`bpChanges` for the BP registers a draw
reads, `GxCommandProcessor::drawsRead`; `xfChanges`; `cpChanges`), and the renderer builds its state (`GxDrawState`)
only when that count moves, applies it once, and lets every primitive under it join one indexed draw. The vertex
pipeline (`GxVertexPipeline`) writes vertices once, straight into the PICA's buffer and already turned for the screen,
with 16-bit indices (so the buffer holds no more than 65,536 vertices, and citro3d wants the indices above it in one
block). Each matrix slot's whole transform (modelview, projection, viewport, EFB mapping, the screen's quarter turn) is
one 4x4, kept until XF changes; the view-to-clip part is rebuilt only when the projection or viewport words do.
Culling keeps GX's sign rule without dividing: with every w positive, a triangle's area in the EFB has the opposite sign
of the determinant of its (x, y, w) rows. Each texgen's texture matrix and dual transform fold into one affine map (its
input's fourth component is always one), kept while the matrices' words are the same; one that normalizes between them
can't fold. Vertex layouts are read once per CP change, with each indexed array's base resolved once (the host's memory
is the Wii's addresses plus one offset) and fixed point applied by multiplying (exact for powers of two). Particles
broke batches another way: each sets its own fading colour in TEV register C0, a combiner constant on the PICA. When a
program reads C0, never writes it and never reads the rasterized colour, C0 takes the rasterized colour's place, in
every vertex's colour, so the program is compiled once whatever C0 holds and a primitive whose C0 alone changed stays in
the batch (the title's draws fell from about 1,000 a frame to 23). Lighting is decoded from XF once per change (the
control words, material, ambient, each light as floats), so a vertex only does the arithmetic, in the same order as
before (bit for bit the same colours); when colour and alpha share their lights, each light's attenuation is worked out
once. The FIFO, which immediate-mode drawing (particles, layouts) feeds a word at a time, used to parse its pending
command again and move it onto itself after every word; now it waits until the length the header gave has arrived.
Measured in the prologue's heaviest frames: draws 26,603 to 135 a frame and 3,410 ms to 552 ms; the title and the file
select about three times faster. What is left is about 1,100 instructions a vertex (decoding, lighting, transform,
texgen): the next step is structural (vertices kept on the PICA's side and transformed by its vertex shader).
The renderer's `gx perf:` line in Azahar's log (every four seconds or more) shows a frame's milliseconds split into
drawing (and how much of it waits for the GPU and vblank), the logic step, animation and the retrace wait
(`GameSystem::frameLoop`, adapted, marks the phases), primitives, how many came without a state change, vertices,
triangles, draws, TEV programs compiled, and the BP registers behind state changes. Azahar emulates the base 268 MHz
clock at about one tick per instruction and charges 150 ticks for each `svcGetSystemTick`, so ticks are read only a
few times a frame; a New 3DS at 804 MHz should run faster than these numbers.

**Meshes the PICA transforms.** Batching left about 1,100 instructions a vertex, and a 3D scene's vertices are almost
all in display lists (J3D draws every shape as one), called again each frame with the same bytes. So the command
processor offers each `GX_CallDisplayList` to the host first (`GxCommandProcessor::Host::displayList`). When the state
is one the GPU path covers and the list holds only primitives, the renderer decodes it once into a mesh
(`GxVertexPipeline::buildMesh`: model-space positions, colours, normals, each texgen's input vector, the vertex's matrix
slot) kept in linear memory, and from then on draws it with the PICA's vertex shader doing the work (`shaders/gx.v.pica`):
each slot's whole transform as uniform rows, each texgen's folded matrix, and lighting. A mesh is keyed by the list's
address, size, first and last words, where its colour comes from, its texgens' sources and whether it is lit, and keeps
the CP registers it was decoded under; only its own formats' tables and the arrays it reads count, since the matrix
arrays change every frame without touching it. The game's cache maintenance (`DCFlushRange`, `DCStoreRange`,
`DCInvalidateRange`, `DCZeroRange`) marks memory as changed through a bitmap of the pages meshes came from and a small
queue the renderer drains; the hook never takes the FIFO's lock, as it can run holding the CPU's (taking it hung the
title). A mesh over changed memory is dropped. Lighting runs in the shader as `GxLighting::apply` does it: up to four
light entries, each a light under one colour part's settings. Spot or specular attenuation and the diffuse function are
numbers mixed in, not branches, so an actor (colour lit by lights 0 and 1, alpha by light 2 with other settings) is one
loop. Positions come prescaled, as the SDK puts directional lights at 1e18 and the PICA's 24-bit floats overflow near
1.8e19 when squared. An instruction's second source can't be a uniform (picasso: "invalid source2 register"), so zero
and one live in a temporary. A lit slot takes ten rows (clip, modelview, normal); four fit, so a lit list naming more
slots is drawn in groups of four (`GxMesh::Group`, its vertices copied per group), each group's draw reserving its own
room in the command list, since `GPUCMD_Add` drops what doesn't fit without a word. `gxMeshModel` is the shader in C++:
host tests check it against the CPU path for point, directional, spot and specular light and for six-bone groups, and
`dev.gx_check=1` checks every new mesh in the game the same way, vertex by vertex and index by index. In the Star
Festival plaza a frame went from 605 ms (CPU path, batched) to about 117 ms, with the picture the same to within 0.6% of
the pixels' range over Mario.

**The FIFO without a lock per word, and copies that stay.** With the renderer switched off (`dev.gx_null=1`) the game's
own work still took 45 to 50 ms a frame, much of it in GX writes: each word took the FIFO's lock (a fair lock built on a
mutex and a condition variable). The lock is now an atomic ticket lock that only waits through the kernel's arbiter, and
the main GX thread's writes gather in a 16 KB buffer as the Wii's write-gather pipe gathers them, handed to the command
processor at `GXFlush` (which the SDK calls before every wait for draw-done or a token), before a peek or a movie frame,
when the copy mode changes and when full; the processor reads a whole flushed buffer in place. The writes themselves are
inline stores (`smg/GxFifoWrite.h`: which thread from the thread-local storage register, then bytes stored big-endian);
another thread, or a full buffer, takes `GxFifo.cpp`'s way. One EFB copy a frame still went through the CPU, reading
the whole target back and encoding it, then decoding it again for the next draw: Mario's shadow, which `MarioShadow`
captures with `JUTTexture::captureDolTexture` into a texture it only draws with. That copy cost 25 ms a frame; it now
stays on the GPU as `JUTTexture::capture`'s do, and the plaza takes about 80 to 85 ms a frame.

**Per-draw work.** What's left is mostly paid per draw, not per vertex (`dev.gx_timing=1` splits it). A mesh draw
sends a uniform row only when it differs from citro3d's copy (every row is sent once at setup, so a skipped row is known
to be on the GPU). A vertex layout follows its arrays' new bases without being read again (J3D's `GXSetArray` before
every shape used to rebuild it). The draw state (`readGxDrawState`, about 12,000 instructions) is looked up by every
register it reads (the BP registers a draw reads, the TEV colours and konsts, the channels' XF words), since J3D's
materials come back every frame; materials whose TEV colours are animated still miss, as their program depends on them.
The plaza's frame is now about 67 ms in Azahar (logic 6, animation 5, drawing 55 of which 7 to 9 wait for the GPU
and vblank).

**The haze.** The Star Festival looked washed out, as if the brightness were turned up. Frame dumps (`dev.gx_dump=1`,
which lists a frame's draws with their state and the EFB's average after each) found it in the bloom's blur: six taps of
one map, more than the PICA's three texture units, and each tap past the third read as one (white). A tap whose map a
unit already samples now shares that unit, which keeps five of the six weights and the picture right
(`CombinerTests` checks a six-tap blur is never brighter than its map).

**A render thread.** `GameSystem::frameLoop` draws (beginRender, draw, endRender), then runs the logic step and
animation, and only then waits for the GPU's draw done (`MainLoopFramework::endFrame`): the Wii's GPU drew a frame while
its CPU computed the next. The port ran its command processor and renderer inside `GXFlush`, on the game's thread, so
none of that overlapped. Now the game thread's inline GX writes go into a 512 KB ring (`GxFifo.cpp`), handed over at
`GXFlush` and every 16 KB, and a render thread on the New 3DS's third core (core 2; else the system core at 80%; else
none, and the old way) reads and draws them. The game waits only where Wii games wait for their GPU (draw done,
tokens) and where the port reads the GPU's results from another thread (peeks, movie frames: they wait for the ring to
drain). The render thread raises the pixel engine's interrupts itself, as the audio thread raises its own, so a
draw-sync callback's `GXPeekZ` runs as its token is read, at the right point in the stream. One difference from the
Wii remains: an interrupt handler runs on its own core while the game thread runs on, where the Wii froze the
interrupted thread; data a game thread shares with a handler under `OSDisableInterrupts` is still exact (the port's CPU
lock spans cores), and the game's draw-sync callbacks only leave values for the next frame's logic. The copy-mode switch
around `JUTTexture::capture` became the port's own command in the stream (0x01) so it stays in order with the copy it
wraps, and a new mesh marks its pages before it is read for good (a write and flush from another thread in between would
otherwise be missed). `dev.gx_inline=1` keeps the old way, for comparison. Azahar emulates both cores on one host thread,
so a run takes longer in wall time; its emulated time is what counts. The frame also stopped waiting for the next
vblank before it began (`C3D_FRAME_SYNCDRAW`): the game paces itself by the retrace, and a frame that ran long lost half
a refresh on average. The Star Festival plaza went from 70-83 to 38-46 ms a frame (timing on), the frame now bound by
the render thread (about 40 ms) while the game's own thread needs about 17.

**Per-draw work, continued.** A state cache hit cost about 29 µs: a 143-word key built by `push_back`, hashed with 64-bit
multiplies (the ARM11 has none), compared through `std::vector`, then a 1.7 KB draw state (with a `std::string` in its
material) copied out. The key is now gathered into an array and hashed with 32-bit multiplies, and the draw uses the
cached state where it lies; the TEV program cache hashes by words too. citro3d marks a whole block of registers to be
sent again for each setter called, so the renderer now remembers what it last gave citro3d (combiner stages, blend,
alpha and depth tests, cull, scissor, texture bindings, fixed attributes, the mesh attribute layout) and calls a setter
only when its value changed. citro3d's `C3D_FrameEnd(0)` flushes the CPU cache over the whole linear heap at every frame
end and every readback (Azahar models no caches, so only a real console pays for it): the renderer flushes what it
writes itself (meshes and textures when made, the frame's vertices before each command list ends), so it now passes
`GX_CMDLIST_FLUSH`. The command processor parses about 200 KB a frame, most of it J3D's material lists of BP loads;
a load now looks up in a 256-entry table whether a draw reads its register and whether it does anything besides being
stored (most don't), and BP is the first opcode it checks. The state cache's key now holds only what the state reads for
the TEV stages in use and the texture maps they sample (`gxDrawStateKey`, about 50 words instead of 127).

**States that differed only in what isn't drawn.** The key still held all eight TEV colour words, the konst words and
the blend register whole. Values earlier draws left in registers no stage reads, a C0 riding in the vertex colour
(read where it is drawn), a C1 in its palette texel and the blend mode's dither bit (the PICA has none) split one state
into many: the cache passed 500 entries at the file select, and in the climb to the castle refilled 1,024 entries
every few seconds, so dozens of states a frame were read again. The key now holds only the colour registers the active
stages read, the konst words only when a stage reads KONST, and a marker for a C0 or C1 that can ride; where
`readGxDrawState` kept the value after all (the riding program would be approximated and the folded one exact), the
entry keeps the register words and holds for that value only. The file select keeps 73 states instead of about 500;
in the climb the cache misses 0 to 4 times a frame instead of 26 to 60, and reading GX per state build halves (12 to 7
µs). The cache is back to 512 entries, and the perf line counts what it keeps and how often it is cleared.

**Fewer, leaner mesh draws.** A lit matrix slot took ten uniform rows (its whole clip transform, modelview and normal
rows), so only four fit and Mario's skinned lists drew in 21 to 34 groups. Lit slots now take six rows (modelview,
normal) after four rows from view to clip space that every slot shares, and the shader goes through view space, which it
needs for lighting anyway: nine slots a draw, so a GX list (ten at most) is one draw or two. When citro3d holds nothing
of the renderer's yet to send (none of its setters called since its last draw), a mesh draw writes its own registers
into the command list: the uniform rows that changed, buffer 0 aimed at the mesh, and the fifteen registers
`C3D_DrawElements` writes for a draw. The uniform values always live in citro3d's own copies (`C3D_FVUnif` and its
kin), sent by citro3d when it has something pending anyway and by the renderer otherwise, so the two never disagree.
Every command list starts with something pending, so its first draw is citro3d's and citro3d submits the list. A
command's header counts at most 256 values; `GPUCMD_Add` splits longer writes and so does the renderer's writer.
`dev.gx_noraw=1` sends every draw through citro3d. One trap found on the way: `C3D_FixedAttribGetWritePtr` takes an
attribute's index (its place among the loaders and fixed attributes, in the order they were added), not the shader
register it feeds. The mesh layout's normal (register 6) is attribute 5 and its matrix slot (register 5) attribute 6;
they had been swapped since the first lit meshes, harmlessly while both fixed values were zero, until a rigid lit mesh
(Mario's hands, on one joint) needed its slot to be 4 and drew from the view-to-clip rows instead: a giant glove over
the camera. The shader's C++ model reads the slot from the vertex, so the mesh check could not see it. `dev.av_sync=1` puts a white square in every 90th drawn frame and a click at the start of the next audio block NDSP takes, at one moment, and logs when each goes out: the port plays the click 38 to 51 ms after its frame goes out. Azahar's video dumper (`-d FILE`) writes the audio track about 675 ms early (clicks land 640 ms before their squares), so a recording's sound is moved back by that much before it is shown.

**The DSP on its own core.** The Wii's DSP ran JAudio2's microcode beside the CPU, answering its mail by interrupt;
the port ran it inside `DSPSendMailToDSP`, on the game's core, about 5 to 7 ms of a plaza frame. Now a thread on the
system core (`APT_SetAppCpuTimeLimit(80)`, core 1; the New 3DS's third core is the render thread's) takes the mails in
order and raises the DSP's interrupts itself. The Wii's mailbox held one mail and its sender spun, interrupts off,
until the DSP took it; spinning here would hold the game's core until the other core got to it, so the queue takes
each mail as it is sent. A mail sent by an interrupt handler running on the DSP's own thread (which raised the
interrupt) runs at once, as that thread can't take its own mail.

**Thirty frames a second.** The game's logic runs 60 steps a second; drawing every one would need about 16 ms a frame.
`MainLoopFramework` already has a render interval (`mRenderInterval`, `mDoRenderFrame`): the adapted
`GameSystem::frameLoop` sets it to two and draws only when the framework says to, so the game runs at full speed and
shows every second step, 30 frames a second (`dev.draw_every=1` draws every step, for comparing the renderer).
The scene's view matrices and draw lists are computed in `calcAnim` (`executeCalcViewAndEntryList`) and its draw lists
are persistent, so a step that isn't drawn leaves nothing behind. The framework waited for the GPU's draw done at the
end of a drawn step, leaving the render thread idle through the next one; now `endFrame` leaves it, and the adapted
`frameLoop` waits in the next step, after its logic and before its `calcAnim`: J3D double-buffers its draw matrices by
`calcAnim`, so that step is the first to rewrite what the drawn frame reads, and the Wii's own loop let the GPU draw
through the logic step in the same way. The Star Festival plaza then runs at 30 frames a second with the game at full
speed in Azahar (33.4 to 34.5 ms a frame, two logic steps each, at the 3DS's slow 268 MHz clock; the render thread busy
26 to 28 ms of each, the game's thread about 25). A real New 3DS runs at 804 MHz; only one can say how much room that
leaves.

**Bitfields read as words, again.** `J3DModelX::drawIn` reads its 32 one-bit draw flags as a word and calls display
list i for each bit i set (lists 0 to 15, built in its constructor: the shadow capture's alpha-only silhouette, the
silhouette through walls, the reflection's settings). Metrowerks keeps field `_k` at bit 31 - k, GCC at bit k, so every
flag Mario's drawing sets (`_10` to `_1E`) landed past bit 15 and none of those lists ever ran: Mario's shadow capture
drew his simple model in colour into a corner of the screen (the black shape over the plaza) instead of only into
alpha. The adapted `drawIn` tests bit 31 - i. `MR::validateExCollisionParts` set the Binder's flag byte by its mask,
0x20, which is `_2` for Metrowerks and `_5` for GCC; it names the field now. A sweep for structs read back as integers
(`*(u32*)&`, `*(u8*)&`, unions pairing bitfields with a word) found nothing else that reads them.

**EFB copies to RG8 and GB8** keep two channels a texel, as the pixel engine writes them (green then red, blue then
green: read back as IA8, alpha byte first); `GxCopyTests` checks both. A copy kept on the GPU goes to the CPU's encoder
for these, as the PICA's copy pass has no two-channel packing. Still left: depth copies, equality compares (the water's
masks), and the second colour channel, which the PICA's combiner has no second vertex colour for.

### Particles: the title and file select

The plaza ran at 30 frames a second while the title screen and file select ran at 7 to 15: the galaxy's dust stream is
about a thousand JPA particles a frame, and each cost the renderer two draws. What each particle sends: its prim and
env colours (`GXSetTevColor` for C0 and C1, each four BP loads, the BG half written three times as the SDK does), its
position matrix (and a texture matrix for some), then `GXCallDisplayList` of a quad from `jpa_dl` (one 32-byte block).
Four changes took the title from 97 ms a frame to 31 to 46:

- **No flush primitive.** Before a list call or `GXBegin` after matrix loads, the SDK's `__GXSendFlushPrim` sends a
  triangle strip of one repeated vertex (all-zero indexes) for the Wii GPU's vertex cache. It covers no pixels and the
  port's GPU path has no such cache; the adapted SDK keeps its bookkeeping (`bpSentNot`) and sends nothing.
- **Small lists join the batch.** A display list in one 32-byte block is read on the CPU path (`MeshSmall`) instead of
  drawn as a mesh of its own, so consecutive particles share a draw.
- **C0 and C1 ride.** C0 already rode in the vertex colour. JPA's TEV blends env to prim by the texture
  (lerp(C1, C0, TEXC)), and the PICA has one vertex colour, so every new C1 still started a draw (about 400 a frame).
  On the CPU path a state may now read C1 from a unit of its own (`compileTev(material, color1FromUnit)`,
  `CombinerProgram::Palette`): each primitive's C1 is one texel of an 8x8 RGBA8 palette texture, taken in order through
  a command list (`startBuffers` starts over), so a recorded draw's texels never change; the vertices carry the
  texel's coordinates. A mesh's state keeps C1 a constant (states are keyed by the kind of draw too), so models drawn
  from meshes are unchanged. Title draws went from about 416 a frame to 25 to 32.
- **Cheaper checks per particle.** `GxCommandProcessor` counts changes beside C0 and C1 and C1's apart, so the ride
  check is two compares; and it counts changes to each four-word row of XF matrix memory, so a texgen's folded texture
  matrix is reused across a particle's position matrix load (`foldStillHolds`).

Then three more, each measured on the title (Azahar, 268 MHz):

- **No whole-piece copies.** `GxCommandProcessor::put` read a piece of the FIFO in place only while nothing was waiting;
  once a 16 KB piece ended inside a command (nearly always), every later piece was copied whole into staging behind it.
  Now a waiting command takes just the bytes it still needs and the rest is read where it is (27 to 24.5 microseconds a
  particle).
- **Vertex setup kept.** `GxVertexPipeline::prepare` redoes only C0's bytes, the texgen folds and the clip matrices
  when nothing but TEV colours or XF matrix memory changed (`xfRegisterChanges` counts register loads apart).
- **Small lists decoded once.** A list in one block is decoded into `GxVertex`es on first use and drawn from them
  (`runDecoded`) until its bytes or the CP registers it was read under differ, or the memory it came from changes
  (the meshes' page marks; dropping every mesh drops these too). 24 to 19 microseconds a particle.

The title then runs at 28 to 30 frames a second (33.4 to 35.8 ms), the file select 26 to 30.

Later: `GXSetTevColor` wrote a colour's BG half three times (the Wii GPU's register latch wants it); the port's processor
ends in the same state after one, so the adapted SDK writes it once. A particle is then 82 bytes of commands (C0 and C1,
two loads each; its position matrix; the list call), and the menus' render time fell from 19.75 to 18.35 microseconds a
primitive: 84 of 92 four-second windows hold 29 to 30 frames a second, the busiest sparkle bursts 27 to 28. Also: a clip
matrix converts its twelve source elements to double once, not four times each, and a decoded list's draw skips the
changed-memory queue's lock when nothing is pending and compares its 32 bytes by words.

A depth peek (the star pointer's `GXPeekZ` at a draw sync each frame) pauses the frame and waits for the GPU, then copies
back only the 8x8 tile of the depth buffer that holds the pixel (256 bytes, `C3D_SyncTextureCopy`); it took 1.2 ms a frame
copying the whole buffer. Checked against the whole readback at 188,700 points, none different.

**Draw syncs before the next step.** Deferring the GPU wait to the next logic step (above) let that step's logic run
before the drawn frame's draw-sync callbacks: with the render thread seconds behind (a scene's first textures), the
storybook's hand-over to the plaza tore down the scene and then ran `TalkPeekZ::drawSyncCallback`, whose
`TDDraw::invProject` read a camera that was gone. On the Wii every callback of a frame ran before the next step. Now
`GXSetDrawSync` counts the tokens set, the render thread counts those it reads, and the step after a drawn frame waits
until it has read them all and then takes any interrupt still waiting for the CPU (`smgWaitDrawSyncs`), so the
callbacks run before the logic. Waiting for the handlers to finish instead froze the title: JAudio2's DSP handler can
wait on a full message queue while holding the port's CPU, and the token's interrupt queued behind it. The plaza keeps
30 frames a second (3 ms to spare). Both waits, this one and the one before `calcAnim`, later gave way to a frame
in flight (below), with the scene's destruction waiting for the callbacks instead.

**A scene's first textures.** The game's textures are decoded and converted on first use, on the render thread; the
plaza's, drawn first behind the storybook, cost about 4 seconds (2.2 Mpixel of CMPR). Three changes made it about a
second: the ETC1 encoder tries the three tables around a half-block's widest offset and picks modifiers by midpoints
(PSNR on the tests' gradient 37.03 to 37.09 dB), takes both splits' averages and errors from the four quadrants' sums
(output identical); `tile()` maps columns once instead of two software divides a pixel; and a new mesh's pages are
marked by a span pass (`meshSpan`) so it is built once, not twice.

**Textures across scenes.** Converted textures are kept by Wii address, size, format and sampler. Until one was copied
over, nothing let them go: a new scene's files read over an old scene's memory left a texture of the same size and
format at the same address showing the old picture, and the map only grew. `JKRDvdRipper` calls `DCInvalidateRange`
over each file it reads and `DCStoreRange` after decompressing, which reach `gxRenderMemoryChanged`, so textures mark
their pages as meshes do and a change over one drops it (the frame so far is ended first, as for a mesh). The perf
line counts textures kept and dropped, and how much of the free store is taken. Converting a model's textures ahead
on core 1 was tried and taken out: 837 textures are loaded by the title (resident particles, layouts and objects) and
the free store has about 12 MB beside the Wii arenas, so results held for later were evicted before their first draw
(14 of 994 used), and a larger budget starved mesh building.

**A frame in flight.** At the hilltop view of the castle the game's thread needed about 27.6 ms of work a frame and
the render thread about 28.4, both under the 33.4 of a 30 fps frame, yet frames took about 38: the step after a drawn
one waited for the GPU before its `calcAnim`, which left the render thread about 23 ms. That wait was there because J3D
double-buffers what the GPU reads by `calcAnim` (each model's draw and normal matrices, its material and packet display
lists), trusting the Wii's GPU to finish a frame within one logic step; with two steps a drawn frame, the second step's
flip lands on the buffer the frame being drawn still reads. So that data no longer reaches the render thread by
address. J3D's indexed matrix loads (`J3DFifoLoadIndx`, `J3DFifoLoadNrmMtxIndx3x3`, and the SDK's `GXLoadTexMtxIndx`)
go out as immediate XF loads of what the index names when they are sent (`smgGXLoadIndexedNow`, reading the arrays
`GXSetArray` gave for 12 to 15), and `J3DDisplayListObj::callDL` and Mario's own two-buffer light and alpha lists
(`J3DModelX::drawIn`) copy their commands into the FIFO (`smgGXCallDisplayListNow`): the same bytes the Wii's GPU read,
as J3D wrote nothing over them until the frame was done. The two places that swap CPU-deformed vertices every step
(`J3DDeformData::deform`, the fur shader) swap only when a frame has been drawn since the owner last wrote
(`smgSwapSinceDrawn`). The frame loop then waits only before drawing, until the frame before is out (every display
copy issued has been read: `smgWaitFramesOut`), so the render thread has a whole frame. The draw-sync callbacks (the
star pointer's and the talk balloons' depth peeks, the lens flare's) run beside the next logic step, as the Wii's
interrupts ran beside its logic; a scene is destroyed only after the GPU is done and every callback has run
(`GameSystemSceneController::exeDestroyScene`), the case that crashed when one ran late. The stream grew from about 93
to 171 KB a frame in the plaza, and the castle view went from 25-29 frames a second to 29-30 (both threads now near
28 ms). Other per-call cuts with it: vertex layouts are kept by their descriptor and table words (J3D sets each shape's
own, so they came back every shape) and read only for the CPU path; lighting is read again only when a light or an XF
register changed, not at every matrix load; a depth tile read back serves later peeks until something is drawn (the
perf line counts peeks and their time, about 1.1 ms each); the game's area lookup by name
(`AreaObjContainer::getManager`, a `strstr` against each of about 40 managers, many times a frame) is remembered by
the name's address and text on the game's thread.

**Work no frame shows.** With every second logic step drawn, the step right after a drawn one computes view matrices
(`J3DModel::viewCalc`, through `ModelManager::calcView`) that the next step computes again before anything draws them:
the frame loop now leaves them out there (`smgViewNeeded`), as their data reaches the GPU when a model is drawn and
J3D's buffers no longer need a fixed rhythm. JMath's inline vector functions (`JMathInlineVEC`) are inline again (the
port had them as calls; inside the namespace they must be called by their full name, or argument-dependent lookup also
finds the SDK's global `PSVEC*` and the call is ambiguous). An inline matrix load is written into the stream in one
pass rather than a word at a time.

**Particles that take turns between textures.** Walking around the castle view, Mario's running dust was ~180 CPU-path
draws a frame, each its own batch: every puff picks one of three textures at random, and a texture change ended the
batch. A CPU-path state with one map on one unit now also gets a base, the hash of its key without that map's size and
address (its GX format stays in, so only textures of one format meet); two states in a row with the same base and
different textures are a turn. Once a base's textures have taken 24 turns within 8 frames, they're packed into an atlas
(at most 8 members, power-of-two sizes, shelves within 256x256, each bordered by the texels its wrap mode samples past
its edges), in the PICA format they share, or LA8 for gray ones, or RGBA8; a later window with new textures rebuilds it
with them while they fit. A state whose texture is in the batch's atlas then joins the batch: the vertex pipeline maps
that unit's coordinates into the texture's place (`setAtlas`). A vertex outside [0, 1] would sample a neighbor: the
atlas is given up for good and the primitive drawn again from its own texture. A base that fails to build three times,
or rebuilds more than six, is left alone; a member's memory changing drops every atlas. Two traps on the way: units
sampling no texture had counted as a second map, and a base was first refused for good after one crowded window (many
effects share the dust's state, some with textures of other formats). At the castle view, 80-110 dust draws a frame join a batch; the walk measured 13 of 19 windows at
29-30 fps (lowest 26.4), against 12 of 19 (lowest 23.7) without.

**Development settings.** Keys in `settings.ini` for development only, written back only when on: `dev.skip_story=1`
skips the prologue's storybook and movies; `dev.gx_dump=1` dumps a frame's draws to the log once a minute;
`dev.gx_null=1` draws nothing, to measure the game's own work; `dev.gx_timing=1` adds the renderer's parts to the perf
line (CPU-path primitives, display lists, texture copies, frame ends, then each per-call step: state builds and how many
were cache hits, the CPU path's vertices and draws, a mesh draw's lookup, uniforms and draw calls, a texture's first
use; each clock read costs Azahar 150 cycles, so it slows what it measures, by up to a fifth with all of them) with the
vertices of refused lists by reason (the perf line names new draw states whose TEV program is approximated, by reason,
without it); `dev.gx_check=1` checks each new mesh against the CPU path; `dev.gx_inline=1` draws on the game's thread
instead of the render thread; `dev.start=GALAXY N` starts that galaxy at its start point N (StartInfo's `MarioNo`), to
reach a stage's later parts without walking there; it also stops the story's queued demos there, as `dev.galaxy` does
(a new file's opening would otherwise meet the garden's start point 1, the after-the-attack spawn, and the attack movie
queued on landing would freeze the opening's arrival cutscene and be frozen by it); `dev.galaxy=GALAXY N` makes the first galaxy after the file select that
galaxy's scenario N (`GameSequenceProgress::requestGalaxyMove`, once the story has named the stage: a new file's first
move names it only in `moveGalaxy`, which also queues the castle's prologue, so the story is stopped there). `dev.warp=X Y Z [SECONDS]` moves Mario there (`MR::setPlayerPos`) once a run, after that much play (2 s unless given), counted only outside the file select (whose Mario is a
stand-in) and outside demos, movies and the scenario's opening, which place Mario themselves. The heartbeat
(every 300th frame) logs where Mario is, for planning a run's route. Azahar runs the 3DS CPU at its base 268 MHz although the port switches a New 3DS to 804
MHz (`osSetSpeedupEnable`), so every millisecond figure here is a slow-clock one; how much faster hardware is can only
be measured on hardware.

**Race ghosts.** A Cosmic Mario race replays a recording (`GhostData*.arc`'s `.gst`) that `GhostPacket` reads a field
at a time, copying each half and word byte by byte in the disc's order; the adapted reader puts them together
big-endian.

**More byte-order traps.** Data the GPU reads must hold the Wii's bytes even when the source wrote it as wider
integers: `MainLoopFramework`'s `clearZTexData` (Z24X8 texels as `u32`s) cleared every frame to near on ARM, and every
3D draw then failed its depth test. The SDK's GX read `GXColor` arguments through `u32` casts (TEV, konst, fog, light
and channel colours), and J3D read texture numbers back from its display lists with a native `u32` load. Region: Petari
matches a build that loads a Mii-name font (`MiiFont.arc`) the US disc doesn't have, and whose `main.dol` never
names it, so the step is skipped when the archive is absent. Checking `main.dol`'s strings settles such questions.

**Bitfields read as words.** `GXSetTevOp`'s presets (`GX_MODULATE`, `GX_REPLACE`, `GX_PASSCLR`, ...) are tables of
`tev_color_env_t` / `tev_alpha_env_t`: bitfield structs read back as register words. Metrowerks packs a struct's first
bitfield into the word's top bits, and GCC on ARM into its bottom bits. The words therefore came out scrambled, and
every `GXSetTevOp` draw ran nonsense stages. The visible case was the picture font's button icons, drawn as white
silhouettes. The adapted `GXSetTevOp` builds each word field by field by name (`smgTevColorWord`, `smgTevAlphaWord`
in `compat/smg/GxPrivate.h`); a test pins `GX_MODULATE` to the hardware's words. Reversing the field order instead
would break the SDK's positional initialisers, which GCC rejects. Any other bitfield struct read as an integer has
the same trap; `io_reg.h`'s `si_comcsr_t` is the only other one in the SDK headers. JAudio's `JASChannel::MixConfig`
is the same trap in a union: written as a word (`0x0150` = bus 1, nibbles 5 and 0), read as `upper:8`, `lower0:4`,
`lower1:4`. On ARM `upper` came out as 0x50, an index far past the DSP's 12-entry bus table, so every voice's sends
went to buffers that don't exist. The adapted header declares the fields low end first for GCC.

**Weak references and the Wii's whole link.** JSystem declares the functions SMG replaces weak
(`JASTrack::channelStart`, `JASBank::noteOn`, `JASWaveArc::setFileName`/`load`), and SMG's replacements live in
`Game/AudioLib/OverwriteJAudio.cpp`. The Wii's linker took every object, so the strong versions always won. GNU ld
extracts a library member only for a strong undefined symbol: the overwrite file sat unused in the game library,
and the two functions nothing else defined stayed *undefined weak*. On ARM a call to an undefined weak function
links as a no-op whose "result" is its first argument. Every sequenced note stored its own track as its channel,
and each note-off "freed" that fake channel, nulling the track's call stack, so the file-select music returned to
address 0. It was found with a GDB hardware watchpoint (Azahar's stub takes them) on the stack slot. The overwrite
files now link whole, and each 3DS executable's link fails by name on any undefined weak symbol outside the
toolchain's optional hooks (`smg_no_missing_weak` in `CMakeLists.txt`). `__cxa_pure_virtual` was one of those
holes too (the toolchain leaves it weak and undefined), so each executable now links its own, which stops by name.

**Sound animations.** An actor archive's `.bas` (`JAUSoundAnimation`) is read in place by `JAUSoundAnimator`, so it
turns native at mount like the BTI, KCL and JPC files: a count, a control word the game leaves null, then 0x20-byte
sounds. The records sit at offset 8 only where pointers are 32-bit, so the 64-bit host audit reads them there by
hand. All 394 disc files audit clean.

**The resident heap is packed exactly.** `GameSystemStationedArchiveLoader` loads the archives every scene shares
(fonts, effects, messages, the Toads', Lumas' and observatory's models) into the stationed GDDR heap, then
`MR::adjustHeapSize` shrinks that heap to what they use: 20,228,736 of its 20,971,520 bytes, nothing to spare. The Wii
never allocated there again, but a BCSV table the port repacks gets its copy "beside" its original, so the first Toad
of the Star Festival asked that full heap for 2,023 bytes. A copy now goes beside its original only when that heap has
room, otherwise into the free store (resident data is never freed, so neither is its copy). A heap that runs dry now
panics with a census of its used blocks by size, naming each mounted archive by its root folder.

**Movies.** The opening, the endings and the final battle are THP movies (`MovieData`, 640x368 at 59.94 fps, stereo
32 kHz): frames of baseline JPEG, 4:2:0, whose scan isn't byte-stuffed (an 0xFF in it is data). The SDK's decoder is
PowerPC assembly over the locked cache, so the port has its own, `smg::decodeThpVideo` (`source/core/Thp.cpp`:
libjpeg's accurate integer IDCT, planes out). `tools/ThpAudit.cpp` decodes all 61,137 frames of the nine movies, and
`tools/thp-audit.py` sets sampled frames against libjpeg's own decoding: their luma is identical. `THPVideoDecode`,
`THPInit` and the locked cache's `LCEnable`/`LCDisable` (the port's decoder needs no scratchpad) are in
`source/compat/Thp.cpp`; the planes go into the buffers the game hands over, which only the port's
`THPGXYuv2RgbDraw` reads. The Wii turned YUV to RGB in four TEV stages whose signed sums the PICA's combiner clamps
away, so the renderer turns each decoded frame into one RGB565 texture (full-range BT.601, which the Wii's TEV
constants work out to exactly) and draws it where the game's quad goes. The game's `THPDraw.c` and the SDK's
`THPAudio.c` build as they are otherwise; the player's headers and each frame's size words, and each audio record's
header, turn native as they arrive. The player preloads 20 frames, each preload's callback restarting the read step; that
step read before checking how many were left, so the last callback set off a 21st read into the first frame's buffer,
which no callback then turned native. The Wii's disc drive was usually slower than the first decode; the SD card never
is, and the first frame's sizes were read big-endian. The step now reads only while frames are left to preload.

**Camera animations.** An "ANDO" file of kind CANM or CKAN (`CameraAnim::loadBin`) is a 0x20-byte header, eight
components' count, offset and (keyed) type words, then a byte count and the values, all big-endian. They turn native at
mount (`.canm`, `smg::nativeCameraAnimation`); before that, the Star Festival's opening camera crashed reading them.
All 190 on the disc (101 in stage archives, 89 in object archives) pass the converter's bounds.

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

`StarPieceShooter::tryShoot` consumes both Wii B hold and trigger states, so R (and the
stylus touch-to-shoot toggle) produce B's trigger on the first pointed sample and hold
while it continues; the original game keeps ownership of shot timing and charge. B is
only reported while the pointer is valid, as with the Wii's IR. The toggle tap itself
never emits B. The camera reset maps to Nunchuk C, jump aliases to Wii A, crouch to
Nunchuk Z, spin to a digital shake, and the D-pad keeps its four original bits. Pointer
recentering is native and never reaches the game.

Aliased physical buttons are combined before transition detection, so pressing B while
holding A does not create a second jump edge, and releasing one does not release the other.

In stylus mode the pointer is on the screen only while the stylus touches it, so the game's "Point at the screen with
[Wii Remote]" banner (`StarPointerGuidance`'s first frame) would appear every time the stylus is lifted; there it never
appears (adapted, asking `smgPointerIsStylus` in `Kpad.cpp`). The other modes keep it, and the blue star's guidance keeps
its own timing.

The port shows nothing about Miis: its RFL is an empty Mii database, the icon picker ends at the character icons (the
Wii's "Mii" placeholder slot led to "If you create a Mii in the Mii Channel, you can use it as your icon!"), and a save
whose icon is a Mii (one from a Wii) takes Mario's icon without the "The Mii that you were using does not appear in the
Mii Channel" notice. The picker's own texts ("Icon", "Select an icon.") never name Miis. The Mii Channel errors (RFL_01,
RFL_02) and the Mii caution can't be reached with an empty database. Real Mii support is a post-1.0 goal.

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

`resource_audit --bmg` turns all three message files (English, French, Spanish) native under ASan and UBSan:
7,392 messages, 306,992 characters and tags in 50 kinds, 3,948 flow nodes, each read the game's way against the
original, none refused and none different.

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

### Disc images

`WiiDisc` reads a disc image the way the game reads its disc: an ISO, or a WBFS through its block map (blocks a
scrubbed dump left out read as zeros). It finds the data partition in the partition tables, takes the title key from
the partition's ticket (decrypted under the common key the caller gives, or given already decrypted), and decrypts
the partition a cluster at a time: 0x8000 bytes stored, 0x7C00 of them data, AES-128-CBC with the cluster's stored
hash bytes at 0x3D0 as IV (`Aes128`, table-driven, about twenty instructions a byte on the ARM11). A wrong key is
caught by boot.bin's Wii magic. `wii_disc_tests` builds a small disc encrypted the same way and reads it back as an
ISO and as a WBFS; on the USA disc, `disc_tool` (a host tool, given wit's decrypted title key) read all 2,371 files,
3,228.2 MB, from the ISO and from wit's scrubbed WBFS, each identical to wit's extraction.

RVZ (Dolphin's format) keeps a partition's data decrypted, with its hashes left out, so it needs no key. The file
holds the disc in groups (by default 128 KiB of disc, 124 KiB of a partition's data), each stored or compressed with
Zstandard (the top bit of its size), a partition's groups beginning with hash exception lists (only for rebuilding
the hashes, skipped here; a stored group pads them to a word). A group may be packed: runs of literal bytes and of
junk, the Wii's padding, regenerated from a 68-byte seed by a lagged Fibonacci generator (j 32, k 521, the third byte
of each word from bits 18-25) skipped forward to the run's place modulo 0x8000, counted without the hashes. These
details follow Dolphin's reader. The decompressor is the caller's (`decompressWith`): the 3DS build links devkitPro's
`libzstd`, and the host tests use a stand-in, checking a made-up RVZ's three kinds of group and its junk against a
generator written the way Dolphin's format notes describe it. `disc_tool --rvz` rebuilds an RVZ from wit's folder
(files where the table puts them, zeros between, no hashes, ticket or junk): of the USA disc, a 3.15 GB file, every
file read back identical.

The 3DS build reads `3ds/SMG-3DS/game.rvz`, `game.wbfs` or `game.iso`, the first it finds on the SD card
(`DiscImage.cpp`): the disc drive's file table and reads, main.dol's tables and the game code for the NAND's home all
come from it, and the extracted files are the fallback. The file is read through `FSFILE_Read`, whose offsets are 64 bits (newlib's `off_t` is 32, and an image runs
past 2 GB); the reader's buffers come from the free store. A WBFS or ISO needs the Wii's common key, the same in
every console, which the port carries as Dolphin does (Jaxi's call). FAT32 caps a file at 4 GB, so the 4.4 GB ISO can't sit on a
3DS SD card; the scrubbed WBFS and an RVZ can.

The portable math tests check tolerances and conventions, not bit-identical Gekko rounding.
The native validation app passes 1,069 math, 37 WPad, 190 BCSV, 1,043 J3D model/texture,
8,810 TEV combiner, 27 original gravity, 27 NameObj, 89 scene and gravity creator, 48 collision, 21 J3D animation, 13 original actor, 6 J3D transform, 6 J3D skeleton, 14 GX command processor, 19 BMG text, 9 colour, TEV and display list word, 7 ETC1 and 8 GPU texture checks on emulated ARM
in Azahar; the GPU checks compare the hardware's ETC1 decoding against ours. Of the disc's
7,780 materials, 6,987 translate to the combiner exactly. No physical console has been tested.
