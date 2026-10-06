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
  tried when reading the previous value is not exact, since it costs a constant slot. An op places only
  the arguments its function reads: a two-argument op's unused third, a zero, once took the constant slot
  too: 314 materials stopped at "more than two constants", now 20, and 268 more translate exactly (the
  electric rails' spark among them).
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
| Allocation | `GameHeap.cpp` | plain `new` from the game's current JKRHeap once the root heap exists (as the original), the free store before; `delete` by address. The port's own long-lived containers use `compat/smg/FreeStore.hpp`'s allocator, and port code the game calls into opens an `smg::PortMemory` scope, so its scratch never spends a game heap (a solid heap would never get it back). Its alignment stays the Wii's 4, where ARM's ABI promises 8: the ARM11 with unaligned access on loads 8-byte values from word-aligned addresses (`LDRD`, `VLDR`, `LDM`; only `LDREXD` wants 8), and 8 would pad heaps sized for the Wii's own use |
| Disc | `Dvd.cpp`, `DiscImage.cpp` | entry numbers from the disc's file table, the RVZ image's or `sys/fst.bin` (not reproducible by sorting: MovieData precedes ModuleData); reads from the image or the extracted files; async reads finish on a worker that calls back like the drive's interrupt |
| Video | `Video.cpp` | a retrace thread on the 3DS VBlank runs the game's retrace callbacks, latches flushed buffers |
| System settings | `SystemConfig.cpp` | language and sound mode from the 3DS's settings; the game's wide mode, adapted to the 5:3 screen (`compat/smg/Screen.hpp`) |
| WiiConnect24 | `Nwc24.cpp` | answers as a Wii with it switched off; the game sends no letters |
| ARAM | `RVL_SDK/aralt/aralt.c` | the SDK's own "Dummy ARAM" in MEM2, compiled as it is |
| Pure software | `Os.cpp`, `Math.cpp` (host too) | OSReport, OSPanic, PPCHalt, cache ops, MSR, error handlers kept; the SDK's matrix helpers |
| GPU (GX, GD) | `RVL_SDK/gx/*.c`, `gd/*.c`, `GxFifo.cpp` | the SDK's own GX and GD, built as C++ with C's conversions; its register blocks are port memory (hardware blocks the port has no counterpart for stop by name); the FIFO takes big-endian bytes into a command processor (`smg::GxCommandProcessor`, portable and tested) that keeps CP/XF/BP state, follows display lists, raises the pixel engine's draw-done and token interrupts, sizes and decodes vertices by the CP's descriptor and attribute tables (the field layout of the SDK's own `private/cp_reg.h`), and hands each whole primitive to the host, which stops there until drawing is written |
| GPU drawing | `GxRender.cpp`, `smg::readGxDrawState`, `smg::GxVertexPipeline` | the draw state (registers through the SDK's own `private/*_reg.h`, TEV to combiner stages) is read again only when a register a draw reads changes; each primitive's vertices are transformed and culled on the CPU, and consecutive primitives under one state are one indexed citro3d draw on the top screen; the display copy ends the frame |
| Audio | `WiiAudio.cpp`, `smg::WiiDsp` | the Wii DSP running JAudio2's own microcode protocol in software (ROM boot, task mails, commands, voice mixing, filters, reverb); the AI's DMA blocks play through the 3DS's DSP (NDSP), raising the AI interrupt per block. NDSP needs `sdmc:/3ds/dspfirm.cdc` (a console's own dump; Azahar's HLE DSP takes any file there): without it the bottom screen says so and the game runs silent |
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
structures native in place where original code first reads them, recognizing data it already turned. RARC
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

Byte order also hides in code that builds data: words written as halves, or halves read as words, are right only on
the CPU they were written for. `DynamicCollisionObj` builds a one-leaf KCL octree by writing 0x8000 then 2 as two
`u16`s; `KCollisionServer::searchBlock` reads them as one `s32`, which is the leaf 0x80000002 on the Wii and the node
0x00028000 here, so every ray through Beach Bowl's water surface walked 0x28000 bytes past the octree into the heap
(an unmapped read in `checkArrow`, seconds after landing). It is written as the one word it is now. Finding it: the
KCL a crash walked matched no file on the disc (`resource_audit --collision` and a walk of all 1,016 disc octrees found
nothing wrong), so the data was built at run time. Bitfields over a word are the same trap: Metrowerks fills them from
the top bit, GCC on ARM from the bottom. `RushEndInfo` sets a rush's damage kind through `mFlags.mDamageType` and
`MarioActorRush` reads it as `(_20 >> 24) & 15`, so on ARM every bind that ends in damage (a pole, a launcher, a
cocoon) read kind 0; such declarations swap places under `__BYTE_ORDER__`, as `JKRArchive`'s file flags do.

**Messages (BMG).** The game reads its text more than one way, so no single byte order serves it. Characters
and a tag's type are 16-bit units; `MessageEditorMessageTag` reads a tag's length and group as bytes, and so
does `TalkNodeCtrl`. Parameters are read as bytes, halves, words or text, depending on the kind. `smg::nativeBmg`
(`source/core/Bmg.cpp`) walks each message from its INF1 offset and plans every unit once: characters and
types are swapped; a tag's (length, group) unit keeps its bytes; parameters follow the game's reader for their
kind. Those are: a wait's `getParam16` (group 1 type 0), a sound or font name's `getParamPtr` text (groups 2
and 10), a player name's or color's `getParam8` (group 5, group 255 type 0), and a number or string tag's
slot plus `getParam32(1)` argument (groups 6 and 7). The game writes the slot natively before it reads it, and
it is empty on the disc. FLW1's message and branch nodes read two halves after the index, event nodes one word.
FLI1 is never read, and on the disc its declared size runs past the file, so only its block header turns. A
kind with parameters the port has no layout for is refused whole, by name: the system group's ruby (its text
sits at an odd byte), group 8's message index and group 11 aren't on the US disc. `resource_audit FILES --bmg`
turns every BMG on the disc and reads each field the game's way against the original: 3 files, 7,392
messages, 306,992 characters and tags, 3,948 flow nodes, 50 tag kinds, none refused, none different.
`MessageData` calls it (`smgNativeBmg`) right after taking the file from its archive. A sound tag's name is text too, and `MR::convertUTF16ToASCII`
took each character's high byte as its first; it reads the unit whole now, or every in-text sound (a Luma's voice) had
an empty name.

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

**Colors as words.** nw4r's `ut::Color` and JSystem's `JUtility::TColor` turned to and from `u32` through
memory, which on the Wii's big-endian CPU packs `0xRRGGBBAA`. On ARM the same code reverses the bytes, so a
literal like `0x000000FF` would have been transparent red instead of opaque black, and J3D's material and
ambient colors (`loadMatColors`, `loadAmbColors`, `J3DGDWrite_u32(*(u32*)color)`) would have gone to the GPU as
ABGR. All three now pack and unpack the value the Wii's way (R in the top byte). The same pattern built every TEV
stage's two BP commands (`J3DTevStage::load`, `*(u32*)&field_0x0`: the register byte, then three setting bytes),
which on ARM would have named a setting as the register; they are packed by value too. `tests/ColorTests.cpp`
pins both, the TEV stage through GD into the port's command processor. `J3DShapeDraw` read each primitive's
vertex count out of a display list as a native `u16`; it is the GPU's big-endian half, so a strip of 3 would
have counted 768 and the walk (`countVertex`, `addTexMtxIndexInDL`, which `ModelUtil` calls) run past the list.
It reads and writes those halves big-endian now. Plain copies between same-typed structs through `u32` keep
their bytes on either CPU and are left alone. Resource colors the game reads as `u32` (BRLYT's `vtxCols` and
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
`SpinDriverPathStorage` keeps each galaxy's launch-star path ranges in one slot per scenario, sized by
`getScenarioNum`, which skips hidden scenarios. That is safe: the game never plays a hidden scenario's own row (below,
`dev.galaxy`). When `dev.galaxy` did, Freezeflame's sixth indexed one slot past the array and Space Junk's, whose row
turns on no layer and so has no start point for Mario, hung looking up the zone of an empty placement row.

**What a sanitizer build finds.** A 3DS build with `-fsanitize=undefined -fsanitize-trap=undefined` (no runtime: each
check that fails is an undefined instruction, which the probe's fault handler names) runs where the host can't. Two
checks stay off: `alignment`, because JSystem's plain `new` gives the Wii's 4-byte alignment everywhere while ARM's ABI
promises 8 (the ARM11 with unaligned access on loads 8-byte values from word-aligned addresses, so it stays, see the SDK
table's Allocation row), and `shift-base`, because the SDK's masks shift negative ints left (`PHY_ADDR_MASK`), which gcc
defines as two's complement. A check gcc proves will fail is compiled as a trap reached from ordinary code rather than
from a check's branch, so the disassembly lists them without running anything. Three were ours (the fourth,
`rbtree_remove`, is libctru's own null-path isolation). `TVec2::subInline` returned a reference to its own local: gcc
passed null in its place, so `MR::calcStarPointerWorldVelocityDirectionOnPlane` read address 4 whenever `SpaceCocoon`,
`SpiderThread`, `PunchingKinoko` or `KoopaFireShort` asked for it (an earlier session had found `addInline`'s twin;
`subOperatorInline` and `scaleInline` had it too, and all five return by value now). The floater's base
`getCurrentVelocity` bound a temporary (no galaxy builds the base alone; a static zero now). `JASHeap::insertChild`
named the next heap's tree through a null pointer (`mTree` sits at 0, so the value was right; spelled out now). Running,
the build stopped at boot in `LayoutCoreUtil::initTextBoxPane`, whose placeholder ended at index 9 of `L"XXXXXXXX"`, one
past its end, in `main.dol` too; ending it at 8 shows the same text. `ub-scan.sh` missed the returned temporaries
because the decomp's include dirs are SYSTEM ones, whose warnings gcc hides: it passes `-Wsystem-headers` now and flags
both on the old headers. Playing on, it stopped three more times. A stream's first channel asks for bus 255
(`JASAramStream::init` sets its mix config to `0xffff`) of `setBusConnect`'s 12-entry table: Nintendo's own, since
`main.dol` holds 0x4861 ("Ha" of a string) past the table there, an address outside the DSP's data memory that mixes
nowhere, so it connects to 0 now. The same function's bus rows were declared `_10[1][4]` ("array size unknown") where
six fill 0x10 to 0x40. And `MultiEmitterCallBack::followSRT` handed `isFollowSRT` an uninitialized `FlagSRT`, whose
bools it keeps when the effect has no auto info (the file is NonMatching, so the Wii's start is unknown; false is what
its fallbacks read).

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

**The C library.** MSL's printf writes nothing for a null `%s` (padded to its width like any string); newlib writes
`(null)`. The game builds archive paths that way (`LayoutHolder`, `ResourceHolder`: four sites adapted before the
general fix). The game's link now wraps `sprintf`, `snprintf`, `vsprintf` and `vsnprintf` (`source/compat/MslStdio.cpp`)
around `smg::mslVsnprintf`, which walks the format and hands each conversion to newlib's own `snprintf`, a null `%s`
as an empty string; everything else stays newlib's, which `MslPrintfTests` holds it to. The game's code is compiled
with those four as non-builtins, as GCC otherwise folds `sprintf(d, "%s", s)` into a `strcpy` the wrap never sees.
`printf` and `fprintf` stay newlib's: they only reach the log.

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
read in the CPU's order). Lighting for color channel 0, after Dolphin's software renderer: ambient plus each masked
light, spot or specular falloff, diffuse none/signed/clamped, then the GPU's `(m * (l + (l >> 7))) >> 8`. EFB copies to
textures: `GxCopy` encodes every color format but RG8 and GB8 as the pixel engine writes them, including the vertical
filter; the SDK's "no filter" setting is the taps (21, 22, 21)/64, since six-bit taps can't pass a row through
unchanged. The renderer reads its target back for each copy and for `GXPeekZ`/`GXPeekARGB` (the screen's way round:
EFB pixel to target index `sx * 240 + (239 - sy)`, checked against a live frame), quantised to the EFB's pixel
format; depth reads back as raw D24S8 words, with the port's nearer-is-larger turned to the Wii's Z. Indirect
texturing has no PICA counterpart (no dependent reads): a stage that only offsets its coordinates is drawn without
the offset, with a one-time note, and set-ups that wrap or chain coordinates or bump alpha stop by name. One kind is
left out instead, with a note and a count in the perf line ("primitives left out"): a stage whose coordinates the
lookup replaces outright (`GX_ITW_0` both ways), a pure dependent read, which drawn would sample one texel everywhere.
That is the depth-of-field blur's last pass (Flipswitch and others), which looks up a blend amount by each pixel's
depth: the screen keeps its sharp image, with no blur by distance. A frame whose
commands outgrow citro3d's 1 MB buffer is ended and begun again with the screen output detached. GX's scissor
(BP 0x20/0x21, less the 342 offset) is scaled to the screen and set on the target the way citro3d's quarter turn
wants it: left = 240 - (y + h), top = 400 - (x + w).

**TEV comparisons.** The PICA's combiner has no compare ops (`GX_TEV_COMP_*`), which compute `d + (a > b ? c : 0)`
and friends. Image effects lean on them: `FallOutFieldDraw`'s screen fill sets alpha to `texture alpha > K0.a ? A0 :
0` and lets an alpha test drop the zeros. With A0 = 0 that pass is invisible on the Wii. Read as an ordinary lerp,
it painted its color over the file select's planets. A compare whose c is zero is exactly d, so it compiles
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
gives: intensity as the PE's studio-range Y (a DOT3 stage), one channel for every component, or color with alpha one
for RGB565. A draw that binds the destination address gets that texture. The Wii's memory is not written, so the CPU
never sees these copies; every other copy (the Message Board photo, `captureDolTexture`) is encoded into memory as
before, which also forgets any GPU copy at its address. The first copy of each kind is read back and set against the
CPU's encoding of the same pixels (the log says by how much, and that a mirrored or upside-down reading would differ
more). Not reproduced: the EFB pixel formats' reduced precision, and filters with more than three taps (those copies
stay the CPU's). Inside a frame citro3d's "sync" transfers only queue, so `readEfb` (CPU copies, `GXPeekZ`/`ARGB`)
ends the frame without showing it, reads, and begins again: on hardware the old readback ran before the GPU had drawn.
The Star Festival's first frames then showed the per-primitive costs: `compileTev` ran for every primitive (46% of the
3DS's CPU after Play This File), so `buildGxDraw` keeps each program by the fields it reads (stage count and stages,
TEV and konst colors, swap tables, each stage's texture), and the ETC1 encoder for CMPR textures fits one split (the one
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
broke batches another way: each sets its own fading color in TEV register C0, a combiner constant on the PICA. When a
program reads C0, never writes it and never reads the rasterized color, C0 takes the rasterized color's place, in
every vertex's color, so the program is compiled once whatever C0 holds and a primitive whose C0 alone changed stays in
the batch (the title's draws fell from about 1,000 a frame to 23). Lighting is decoded from XF once per change (the
control words, material, ambient, each light as floats), so a vertex only does the arithmetic, in the same order as
before (bit for bit the same colors); when color and alpha share their lights, each light's attenuation is worked out
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
(`GxVertexPipeline::buildMesh`: model-space positions, colors, normals, each texgen's input vector, the vertex's matrix
slot) kept in linear memory, and from then on draws it with the PICA's vertex shader doing the work (`shaders/gx.v.pica`):
each slot's whole transform as uniform rows, each texgen's folded matrix, and lighting. A mesh is keyed by the list's
address, size, first and last words, where its color comes from, its texgens' sources and whether it is lit, and keeps
the CP registers it was decoded under; only its own formats' tables and the arrays it reads count, since the matrix
arrays change every frame without touching it. The game's cache maintenance (`DCFlushRange`, `DCStoreRange`,
`DCInvalidateRange`, `DCZeroRange`) marks memory as changed through a bitmap of the pages meshes came from and a small
queue the renderer drains; the hook never takes the FIFO's lock, as it can run holding the CPU's (taking it hung the
title). A mesh over changed memory is dropped. Lighting runs in the shader as `GxLighting::apply` does it: up to four
light entries, each a light under one color part's settings. Spot or specular attenuation and the diffuse function are
numbers mixed in, not branches, so an actor (color lit by lights 0 and 1, alpha by light 2 with other settings) is one
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
another thread, or a full buffer, takes `GxFifo.cpp`'s way. A matrix loaded whole (`GXLoadPosMtxImm`, `GXLoadTexMtxImm`:
Dusty Dune's sand sends one per particle) goes in with one look for room (`smgGXFifoXfLoad`), its write point held in a
local, as a store through the byte pointer could alias the global one. Every other command does the same: a BP, CP or XF
load, a display-list call, a vertex's values of one type, a TEV color's two halves, a texture's six registers, a light
and J3D's array bases and matrix indices each look for room once (`smgGXFifoPut2`..`Put4`, `smgGXFifoRun`), where every
byte or word used to read the thread register and three globals; a command that doesn't fit goes a piece at a time as
before, so the ring turns over and hands over at the same bytes (`tests/GxFifoWriteTests.cpp`, and an ARM harness that
hashed 2.3 MB of the SDK's own commands both ways). About 40% fewer instructions per write; Cube Bubble's draw step went
from 3.3 to 2.4 ms. One EFB copy a frame still went through the CPU, reading the whole target back and encoding it, then
decoding it again for the next draw: Mario's shadow, which `MarioShadow` captures with `JUTTexture::captureDolTexture`
into a texture it only draws with. That copy cost 25 ms a frame; it now stays on the GPU as `JUTTexture::capture`'s do,
and the plaza takes about 80 to 85 ms a frame.

**Per-draw work.** What's left is mostly paid per draw, not per vertex (`dev.gx_timing=1` splits it). A mesh draw
sends a uniform row only when it differs from citro3d's copy (every row is sent once at setup, so a skipped row is known
to be on the GPU). A vertex layout follows its arrays' new bases without being read again (J3D's `GXSetArray` before
every shape used to rebuild it). The draw state (`readGxDrawState`, about 12,000 instructions) is looked up by every
register it reads (the BP registers a draw reads, the TEV colors and konsts, the channels' XF words), since J3D's
materials come back every frame; materials whose TEV colors are animated still miss, as their program depends on them.
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

**States that differed only in what isn't drawn.** The key still held all eight TEV color words, the konst words and
the blend register whole. Values earlier draws left in registers no stage reads, a C0 riding in the vertex color
(read where it is drawn), a C1 in its palette texel and the blend mode's dither bit (the PICA has none) split one state
into many: the cache passed 500 entries at the file select, and in the climb to the castle refilled 1,024 entries
every few seconds, so dozens of states a frame were read again. The key now holds only the color registers the active
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

**Lines and points.** GX draws them; the PICA draws only triangles. On the CPU path each becomes a quad as wide as
GX makes it in EFB pixels (`SU_LPSIZE`, BP 0x22: line width and point size in sixths of a pixel): a point a square
around its vertex, a line a band widened across its minor axis, as the Wii's rasterizer does. JPA's point and line
particles spread the texture across them (`GX_TO_ONE`): where a coordinate takes the offset (`SU_SSIZE` bits 18 and 19,
`GXEnableTexOffsets`) the point's right and bottom corners add it to s and t, a line's one side to s. The vertices are
transformed into the tail of the room the renderer gives the primitive (four a vertex for these) and the quads written
from its front, each before the sources it would cover are read. Flipswitch was the first galaxy to stop on them.

**Water in passes.** The game draws its own water (`OceanBowl`, `OceanRingDrawer`, `OceanSphere`, `WaterRoad`,
`WhirlPool`) with GX calls, not models: two scrolling samples of one texture multiplied, C0 added where that
product's red equals A0 (an R8 equality compare: the bright wavy lines), then the screen behind, warped, times C1.
Beach Bowl's (`OceanBowl`) also takes its alpha from a mask: four samples for three PICA units, so the screen was read
as white and the water came out the mint of C1. An exact 8-bit equality needs more combiner stages than the PICA has.
`splitTev` turns such a material into passes whose blended results add up to it, since the blend is linear in the
color: the first is the material without the compare (and without additive terms moved out while it needs more than
three units); a moved term is drawn alone under the whole alpha chain, added times its alpha (blend source alpha,
one); the compare's pass trades color for alpha: its alpha runs the stages before the compare on their reds (a swap
table makes alpha read red and color read alpha), its color runs the alpha chain and takes in the compare's c, and an
EQUAL alpha test keeps only the lines (blend one, one). Only where GX's sum saturates do the passes differ, as they
saturate apart. `CombinerTests` checks Beach Bowl's water this way against the TEV, on and off the lines. A pass that
samples what the program does (the lines' pass) draws the same vertices again, a batch's or a mesh's; one that samples
other coordinates (Beach Bowl's screen term) takes the CPU path beside the program's vertices, its indices drawn once
with the batch: it takes the program's clip positions and colors (the same state but for the program) and works out
only its own texture coordinates, reading just the position and normal when its texgens use only those
(`GxVertexPipeline::runPass`; lines, points and atlases still remake everything). Standing at Beach Bowl's start that
took the CPU path from 13.1 to 12.1 ms a frame. The ocean (`OceanBowl`: 24 strips of 50 vertices, position and color
direct, four texture coordinates by INDEX16, one of them normalized through the dual transform) cost about 880
emulated cycles a vertex, measured instruction by instruction on the host: the normalized texgen re-read 24 XF words
a vertex, every attribute was decoded whether used or not, and an unlit channel still ran the lighting. Now a
`GxVertexLayout::Plan` decodes, an attribute at a time over 16-vertex chunks, only what the primitive reads (a
remade pass only its own texgens' sources); texgen folds are kept per coordinate (a program and its pass took turns
on the same slots, refolding at every prepare), a normalized texgen reads its two matrices as floats (an identity
post matrix only adds zeros, so it is skipped); an unlit channel takes its color straight; a strip with nothing
culled writes its indices directly. The output is the same bit for bit (a fuzz of 288,000 random primitives against
the old code, and the ocean's frame hashed on x86 and on Azahar); the ocean went from 7.9 to 4.1 ms a frame, and
standing at Beach Bowl's start the CPU path from 12.1 to 8.2 ms; at 400x240 its lines come out as scattered bright pixels (how closely they match the Wii's is unchecked).
`OceanSphere` (Drip Drop, Good Egg, Bonefin) blends between two colors by the product instead of adding to it, which no
added pass reproduces; `equalLine` draws it in one program instead (below).

**Environment maps on skinned models.** J3D's shiny skinned models give a texgen a matrix per vertex: the vertex's own
modelview, or the texture matrix paired with its slot (30 rows on, which J3D loads per draw matrix), then the dual
transform. Those lists took the CPU path; at the Comet Observatory 27 a frame, 8,400 lit vertices, most of the
renderer's time there (the perf line's texgen counts found every one of them is one of those two). A lit, skinned mesh
now keeps such a texgen when its vertices all pick the same kind: the shader takes three rows at the slot's base plus
`slotrow` (0 for the modelview, its paired rows after the normal rows otherwise), and its texgen rows become the dual
transform alone (with two rows, the third output's column folded into the offset, since GX makes it one). A paired
unit adds three rows to each slot, so a group holds six slots, or four with two.

**Ambient from the vertex.** A lit channel can take its ambient from the vertex color instead of a register (GX_SRC_VTX
for the ambient). Those lists took the CPU path: at Dusty Dune about 16,900 lit vertices a frame, and 8 to 11 fps. The
mesh now carries the color whenever a lit part's ambient comes from it, and the shader starts that part's sum from
it (`ambrgb`, `ambalpha`), as `GxLighting::apply` does; Dusty Dune runs at 24 to 30. `dev.gx_check` compares positions
against w, as the screen sees them: a far mesh's rounding (0.015 at a w of 48,000) is large in clip units but far
below a pixel.

**Mesh memory, and meshes that move.** Meshes live in linear memory: always up to 12 MB, and past that while 8 MB
stays free for textures and movies. At Toy Time they reached 14.5 MB, free memory never under 8 MB (the textures take
most of it); held to 12 MB, 128,000 vertices a frame had gone to the CPU path there, in frames of 550 ms. A new mesh
with no room evicts the meshes drawn least recently (never one drawn this frame or the last, which the GPU may still
read), and a list refused for room keeps its mesh's size, so it is built again as soon as there is room and no
sooner. A mesh keeps only what its draws read (`GxMeshLayout`, chosen from the state its list is drawn under): the
position, the color when it comes from the vertex, each texgen input that is data of its own (two floats unless it
takes three; one that is the position or the normal reads those bytes again through a second buffer), the normal when
lit or read by a texgen, the slot for per-vertex matrices; what the shader reads and a mesh lacks is a fixed attribute.
A vertex went from 68 bytes to 34 on average over the disc's models (36 at Toy Time). Toy Time's ~13 MB had sat at the
budget, and a restart after a fall, whose new textures took the last of the room, evicted 200 meshes during the black
fade, the start area's first (drawn least recently), to build 151 of them again, byte for byte the same, as the scene
came back: two seconds near 20 fps. Its meshes now take 10.3 MB with 11.5 MB free, and the restart rebuilds none. A texture, movie
frame or copy target that finds no linear memory lets go the meshes the GPU no longer reads, then (unless it comes
inside a draw, whose mesh or batch that would pull out from under it) every mesh, trying again after each, before it
stops by name; no run so far has needed that. Some lists keep their bytes while their arrays move: J3D's deformed
models (Honeyhive's swaying flowers and grass) draw from vertices written into one of two buffers in turn, and
instances of one model share a list, each with its own deformed arrays. Such a mesh keeps its triangles and is decoded
again (`GxVertexPipeline::refreshMesh`: each vertex taken from the list by the mesh's origin map, keeping its slot)
whenever its arrays move and once a frame, into an arena owned by the frame's parity (1 MB each, outside the mesh
budget, reused from the start two frames later). A refresh reads the list's vertices 32 at a time, one attribute at a
time (a `GxVertexLayout::Plan` of fetchers made for each attribute's kind, format and components) and packs them as the
build does, writing every byte the build writes: an earlier refresh left untouched the bytes a build leaves zero (an
unused color, a unit past the material's), so over a reused arena 3,264 of 3,312 refreshes differed from their
rebuilds. An unlit mesh's vertices keep the list's order and are packed straight into place; a lit one's groups gather
them; and a command list's refreshes flush the cache once, with its vertices, not once each. At Honeyhive's start
that is 69 refreshes a frame (6,378 vertices), about 280 to 154 ARM instructions a vertex, and the vertices its moving
lists sent to the CPU path stay on the GPU. `dev.gx_check=1` checks every refresh bit for bit against the mesh built
anew (59,383 at Honeyhive, none differing) and every new mesh against the CPU path.

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
drew his simple model in color into a corner of the screen (the black shape over the plaza) instead of only into
alpha. The adapted `drawIn` tests bit 31 - i. `MR::validateExCollisionParts` set the Binder's flag byte by its mask,
0x20, which is `_2` for Metrowerks and `_5` for GCC; it names the field now. A sweep for structs read back as integers
(`*(u32*)&`, `*(u8*)&`, unions pairing bitfields with a word) found nothing else that reads them.

**EFB copies to RG8 and GB8** keep two channels a texel, as the pixel engine writes them (green then red, blue then
green: read back as IA8, alpha byte first); `GxCopyTests` checks both. A copy kept on the GPU goes to the CPU's encoder
for these, as the PICA's copy pass has no two-channel packing. **Depth copies** (the EFB's pixel type Z24) that stay on
the GPU take one of z's bytes (Z8, Z8M, Z8L): the depth target is copied raw into the scratch texture, read as RGBA8
(a D24S8 word puts z's top byte in G, then B, then A), and drawn through one stage of one-minus that byte, sampled
nearest, or halved with the bilinear sample at each 2x2 block's center (the byte averaged alone, within one of z's own
average's; `ImageEffectLocalUtil::capture` hands its clear flag to `JUTTexture::capture`'s mipmap, so the game's Z8
copies are all halved), as GX's z is 0xFFFFFF less the port's nearer-is-larger, byte for byte; they are checked once
against the CPU's encoder like the color copies. The rest go to the CPU's encoder from the depth target read back
(raw D24S8): Z4 and Z8 take z's top byte, Z8M and Z8L its middle and low, Z16 its top two big-endian, Z16L its low two,
Z24X8 all of it in RGBA8's two blocks; the copy filter blends color only, and a halved copy averages z as one number,
not byte by byte (`GxCopyTests`). A color format stops by name. Flipswitch's depth-of-field blur takes one for its
indirect offsets, which the port leaves out, so only the copy's clear reaches the screen there. Still left: equality
compares (the water's masks), and the second color channel, which the PICA's combiner has no second vertex color for.

### Particles: the title and file select

The plaza ran at 30 frames a second while the title screen and file select ran at 7 to 15: the galaxy's dust stream is
about a thousand JPA particles a frame, and each cost the renderer two draws. What each particle sends: its prim and
env colors (`GXSetTevColor` for C0 and C1, each four BP loads, the BG half written three times as the SDK does), its
position matrix (and a texture matrix for some), then `GXCallDisplayList` of a quad from `jpa_dl` (one 32-byte block).
Four changes took the title from 97 ms a frame to 31 to 46:

- **No flush primitive.** Before a list call or `GXBegin` after matrix loads, the SDK's `__GXSendFlushPrim` sends a
  triangle strip of one repeated vertex (all-zero indexes) for the Wii GPU's vertex cache. It covers no pixels and the
  port's GPU path has no such cache; the adapted SDK keeps its bookkeeping (`bpSentNot`) and sends nothing.
- **Small lists join the batch.** A display list in one 32-byte block is read on the CPU path (`MeshSmall`) instead of
  drawn as a mesh of its own, so consecutive particles share a draw.
- **C0 and C1 ride.** C0 already rode in the vertex color. JPA's TEV blends env to prim by the texture
  (lerp(C1, C0, TEXC)), and the PICA has one vertex color, so every new C1 still started a draw (about 400 a frame).
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
  when nothing but TEV colors or XF matrix memory changed (`xfRegisterChanges` counts register loads apart).
- **Small lists decoded once.** A list in one block is decoded into `GxVertex`es on first use and drawn from them
  (`runDecoded`) until its bytes or the CP registers it was read under differ, or the memory it came from changes
  (the meshes' page marks; dropping every mesh drops these too). 24 to 19 microseconds a particle.

The title then runs at 28 to 30 frames a second (33.4 to 35.8 ms), the file select 26 to 30.

Later: `GXSetTevColor` wrote a color's BG half three times (the Wii GPU's register latch wants it); the port's processor
ends in the same state after one, so the adapted SDK writes it once. A particle is then 82 bytes of commands (C0 and C1,
two loads each; its position matrix; the list call), and the menus' render time fell from 19.75 to 18.35 microseconds a
primitive: 84 of 92 four-second windows hold 29 to 30 frames a second, the busiest sparkle bursts 27 to 28. Also: a clip
matrix converts its twelve source elements to double once, not four times each, and a decoded list's draw skips the
changed-memory queue's lock when nothing is pending and compares its 32 bytes by words.

A depth peek (the star pointer's `GXPeekZ` at a draw sync each frame) pauses the frame and waits for the GPU, then copies
back only the 8x8 tile of the depth buffer that holds the pixel (256 bytes, `C3D_SyncTextureCopy`); it took 1.2 ms a frame
copying the whole buffer. Checked against the whole readback at 188,700 points, none different. Peeks answered from
the frame before, and copies the GPU keeps, queue their transfers behind the draws: citro3d's GX queue holds 32 jobs
until the next `C3D_FrameBegin` empties it, a copy or a peek takes two (the command list so far, then the transfer)
and a frame's end two more, so `roomInQueue` ends and begins the frame again before a full queue would panic in
libctru's `gxCmdQueueAdd` (Bowser's second battle, whose effects copy that often in one frame).

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
their pages as meshes do and a change over one drops it. A dropped texture is kept aside until the next
`C3D_FrameBegin` (which waits for the GPU to finish what was queued) and freed then, so the frame isn't ended for it:
Snow Capsule's `SnowFloor` redraws a 128x128 I8 footprint texture every frame, and those pauses were 1.8 ms a frame.
A power-of-two I8 texture also goes straight into LA8 (`tileIntensity8`, both bytes the intensity, rows from the
bottom in 8x8 Morton tiles) instead of being decoded to RGBA and tiled: Snow Capsule's texture work went from 8.5 to
3.5 ms a frame (its game logic, 10.6 ms a step, still holds it at 28 fps). A texture the game samples between mip
levels (the min filter's low bits; 624 of the disc's 7,311 model textures carry levels) gets them down to 8x8, the
smallest the PICA samples, as one chain in level 0's PICA format with its LOD bias (`picaMipLevels`, `gxLevelOffset`):
the 3DS's screen shrinks textures more than the Wii's did. The perf
line counts textures kept and dropped, and how much of the free store is taken. Converting a model's textures ahead
on core 1 was tried and taken out: 837 textures are loaded by the title (resident particles, layouts and objects) and
the free store has about 12 MB beside the Wii arenas, so results held for later were evicted before their first draw
(14 of 994 used), and a larger budget starved mesh building.

**Work the game does for nothing.** Every logic step `WPadHolder` updates two acceleration trackers on each of the four
channels (the remote and a Nunchuk), three channels with nothing on them, and each averages and turns over its last
128 samples (`WPadAcceleration`, about a tenth of Snow Capsule's game thread by the sampler). Their results read
nothing but that window, so while its 128 samples are one value the adapted `update` keeps them (counting equal
samples in a row as they come) and only runs the stateful `updateIsStable`: the same results, bit for bit. Logic went
from 10.45 to 10.1 ms a step at Snow Capsule (which now holds 30 fps in most windows) and from 5.5 to 5.1 at the
Observatory. Area managers found by name (`AreaObjContainer::getManager`, memoized per name address) match a miss as
a prefix compare instead of `strstr` over the whole name. A quarter of Dusty Dune's game thread is its blowing sand
(`JPADrawDirection`, per particle); the vector `MR::isNearZero` each one asks is inline now (the same six compares),
which took its draw from about 4.25 to 4.1 ms a step.

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
copy issued has been read: `smgWaitFramesOut`), so the render thread has a whole frame. The perf line keeps the waits
on the render thread apart: for ring room, for the frame before, and for everything sent to be drawn (a peek or a
movie frame from any thread but the render thread's). The draw-sync callbacks (the
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
J3D's buffers no longer need a fixed rhythm. The same holds for each model's material display lists
(`ModelManager::updateDL`: material controllers, animated colors and texture matrices, the lists patched): the
controllers set values and never add to them, and the animations' frames advance in `calc`, which still runs. At the
Comet Observatory, Mario standing still, a step's animation went from 8.3 ms to 6.1. JMath's inline vector functions (`JMathInlineVEC`) are inline again (the
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

**Where the render thread's time goes.** Reading the clock costs Azahar 150 cycles, so `dev.gx_timing` slows what it
times by a fifth, and Azahar's GDB stub makes its own clock jump while it samples (frames that read four seconds apart):
neither measures. `dev.gx_zones=1` does instead: the render thread writes one byte saying what it is doing
(`smg/GxZones.hpp`: waiting, parsing, BP runs, XF loads, a list checked, a state built, the pipeline prepared, uniforms, a
mesh draw, CPU-path vertices, a batch drawn, peeks, a frame ended...), and a thread on the system core reads it every
200 µs; the perf line shares each window's milliseconds out by zone. A store per zone costs nothing measurable (34.2 ms
a frame without it at the Comet Observatory, 33.8 with). The GDB sampler still gives a function-level lead when it
follows one thread (`rsp_sample.py`'s `TRACK=NAME`, the thread whose stack shows NAME), read as functions rather than
instructions: the stub halts at block boundaries, so a tiny function called constantly takes its neighbors' samples.

**Fog.** SMG's fog is GX's linear fog on 268 materials (a scan that reads the second byte of J3D's fog info as a switch
finds none: that byte is `mAdjEnable`, the range adjustment, and the type alone says fog), mostly a faint white haze
from 0 to 200000, a few enemies that fade to white within 5000 units and `PlanetDisk`'s blue; and code sets it: Mario's
damage, star piece and dash flashes (`MarioActor::updateLightDL`, `calcFogStartEnd` centers the ramp on his own depth so
his whole model takes one share), the power-up (`J3DModelX`), fur and water plants. The renderer reads BP 0xE8 and 0xEE
to 0xF2 at each state's build (they rebuild the state but are not in its key, as Mario's flash changes them every frame)
and sends them to the PICA's fog unit when they change: `smg::picaFogTable` (`GxFog.cpp`) samples Dolphin's software fog
(`Tev.cpp`: `ze = A / (B - (Zs >> shift))`, less C, clamped, then the exponential curves) at 128 steps of screen depth,
which the fog unit indexes directly with its z flipped, because the port's PICA depth is `1 - Zs / (2^24 - 1)`
(`GxVertexPipeline`'s clip rows). A step is a straight line and most of a scene lies past depth 0.99, inside the last
step, where far fog climbs steeply: the plain chord fogged a far object at depth 0.999 by 42% where GX gives 4%. So each
step runs to the next value unless it would fog more than GX by over 6/256 somewhere, and then only as steeply as stays
within that; too little far fog is the error left. citro3d sends fog (a change makes the next draw citro3d's, which
writes the table), the raw `TEXENV_UPDATE_BUFFER` write keeps the fog bits citro3d set, and the port's own copy and
movie draws and the frame's end turn it off. With fog forced on every draw, near ground stays clear and the castle goes
white, as it should.

**Per-draw work, counted.** The zones say where the render thread's time goes; instruction counts say why. GxRender.cpp and
the command processor were built as the 3DS build builds them, with stand-ins for citro3d and libctru, and a J3D-like frame
(450 materials, 740 mesh draws, 270 particles, about 300 KB of stream) was replayed on an emulated ARM11 that counts each
function's and zone's instructions and hashes every call the PICA would get, so a change has to leave the hash as it was.
The frame took 9.05 million instructions and the biggest costs were small ones paid per command and per word: newlib's
`memcmp` goes a byte at a time (state keys, combiner stages, mesh keys: 7%), a word of XF matrix memory took 24
instructions, each CP load went through the command dispatch alone, and a state build hashed and compared its 50-word key
in two passes. Now a run of CP or XF loads is one loop and matrix memory goes a row at a time (`xfRowChanges` counts the
rows a load changed, not its words; everything compares it for equality), `sameWords` replaces `memcmp`, plain BP loads
skip the mask and color checks, the state cache and the mesh cache have a direct-mapped index in front of their maps (a hit
no longer divides), a state entry keeps its scissor, the pixel state a draw sends is the entry's own (copied only when
`keepDstAlpha` changes it, and `sent.was` brought up a part at a time), the lighting's rows are packed when it is read, not
at each draw, and a mesh draw finds them (`lightingEpoch`) and the view-to-clip rows (`toClipEpoch`) already set when
nothing they are made of changed. The frame is 7.12 million instructions: the state zone 37% less, parse 49%, apply 36%,
XF 23%, uniforms 20%, mesh draws 16%. What is left is mostly the stream itself (BP loads 9%, XF 12%) and a draw's
rows: unchanged BP loads cost about 21 instructions each and matrix rows change with every shape.

**The CPU vertex path, counted.** The same ARM11 counting, over `GxVertexPipeline` alone on streams like the game's: a JPA
quad, lit strips with an environment map, `OceanSphere`'s direct float strips (Drip Drop's water), an indexed skinned
mesh. Most of a particle's 592 instructions a vertex were not vertex work: `run` zeroed a 1,728-byte chunk of sixteen
vertices for each four it read (218 a vertex), built its read plan again (41), and read a color the channel never takes.
Now the chunk is left as it is and only what the plan doesn't write (an attribute the format lacks, a narrow one's last
components) is zeroed; the plan is kept while the CP registers stay; the color is read only where a channel part takes its
material or a lit part its ambient from the vertex. Vertices are then worked out a stage at a time over the chunk, each
stage's constants in locals (built with `-fno-strict-aliasing`, every float stored into a vertex made the compiler load the
matrix and the texgen's coefficients again): position, color, then each texgen as one loop. A folded texgen whose third row is (0, 0, 0, 1), or
(0, 0, 1, 0) over a third input of one (the ocean's identity 3x4), never divides, so its q isn't worked out at all. An
unlit channel with its material in registers copies four constant bytes; a lit vertex's position and normal matrices are
read once per matrix rather than once per element, and lighting walks a list of each part's lights instead of testing
eight mask bits. Instructions a vertex, from the data and from decoded vertices: particles 592 and 257 to 302 and 217,
lit strips 939 and 622 to 515 and 400, the ocean 371 and 277 to 155 and 102 (its second pass, `runPass`, 280 to 111),
indexed skinned strips 352 and 231 to 204 and 123. Every `GxDrawVertex` byte is as it was: 300,000 random scenes (every
attribute's type, format, count and shift, matrix indexes, texgen source, form, projection and matrix, dual transform,
lighting, primitive kind, VAT and array changes between draws, NaNs and infinities) through `run`, `runDecoded` and
`runPass` hash the same as the old code on x86, and 3,000 on the ARM11 build, except for which NaN's sign a sum keeps,
which the compiler chooses.

**The Comet Observatory at 30.** At the session 8 build the Observatory ran 26 to 29 frames a second with the render
thread busy 34.2 ms a frame. In order, each measured on the same standing-still route: the commonest commands are read
without a call (`GxCommandProcessor::command` takes BP runs, CP and XF loads itself; BP runs keep the mask and change
counts in registers, TEV colors too, only events leaving through `loadEvent`), and the per-command tally only the timing
prints is kept only under `dev.gx_timing` (34.2 to 31.9 ms); depth peeks stop nothing (below; 30.7 ms); mesh draws
write their changed state themselves (below; 26.7 ms, mesh draws 6.0 to 2.2 ms). That left the game's thread the limit:
its drawing put J3D's display lists into the stream with newlib's `memcpy`, which goes a byte at a time whenever either
pointer is off a word boundary, and the write point nearly always is (commands are 1, 5 or 9 bytes): `smgGXFifoBytes`
copies words through unaligned stores now, and a matrix (`GXLoadPosMtxImm` and its kin) looks for room once rather
than once a number (`smgGXFifoFloats`). Drawing went from 4.35 to 3.46 ms a logic step, and the route showed all 40
windows at 29 to 30 (lowest 29.5). The game also ends each prohibited section (`MR::ProhibitSchedulerAndInterrupts`,
one per material whose display list is patched) with `OSYieldThread`, a kernel call here; the port's scheduler never
holds a thread back in those sections (`OSDisableScheduler` only counts, and a higher thread waiting for the CPU runs as
the section lets it go), so the adapted destructor leaves the yield out: another millisecond a logic step.

**Depth peeks that stop nothing.** The star pointer and a talking NPC read the depth under a point from draw-sync
callbacks (`StarPointerPeekZ`, `TalkPeekZ`, on the render thread as it reads their token) and use the answer the frame
after. Each read paused the frame, waited for the GPU and copied an 8x8 tile back. Now a peek queues that tile's copy
behind what is drawn (`C3D_FrameSplit`, then `GX_TextureCopy` into one of eight small buffers; the frame's queue runs at
its end, and `C3D_FrameBegin` waits for it) and is answered from the newest finished copy of the same tile, from this
frame or the last; a tile with none yet is read as before. About two of the Observatory's 2.6 peeks a frame come from a
copy; the pauses went from 0.9 to 0.1 ms. `BrightObj`'s color peeks (the sun's brightness) still read the whole color
buffer back.

**Mesh draws past citro3d.** A mesh draw whose pending state was more than its uniforms went through
`C3D_DrawElements`, whose context update re-checks every block and walks 96 uniform flags twice (vertex and geometry
shaders), each command through `GPUCMD_Add`. Now, when all that is pending is the combiner, fragment tests, culling,
textures, fixed attributes or the mesh layout, the draw writes those registers itself (`writeChangedBlocks`), as
citro3d's binders write them: each changed TEV stage's five words, the buffer's update bits and color, alpha test,
blend and depth/write mask (with the gas depth function citro3d writes beside it), the cull mode, each changed unit's
border, size, parameters, LOD and address with its type and then the units' configuration (the cache cleared), each
changed fixed attribute's index and its three packed 24-bit words, the attribute formats and permutation. citro3d's
own copies are still set, so its next draw sends them again and its state stays the truth; the scissor and a fresh
command list still go through it. At the Observatory one group a frame of about 300 is citro3d's now.

**Steps whose result is fixed.** A TEV program the PICA can't fit in six combiner stages was cut short, and what was
left could compute something else entirely: Buoy Base's Gearmos (`caretaker.bdl`'s `BodyMat_v`, their tint a TRK1 color
animation) drew black wherever their soot texture is clear. Their last alpha saturates whatever it reads (4 x (0.42 +
a value of at least 0)), so the stages before it that only fed that alpha are dead. When a program comes out
approximate, `Compiler::foldFixed` rewrites each part whose range (`stageRange`) is a single 0 or 1 after its clamp to
read that constant, `clearDead` drops what only it read, and the material is compiled again. The register wrap that a,
b and c read (a register's low eight bits) is kept continuous like the rest of the compiler's arithmetic, as the tests'
TEV model keeps it, so a computed value needn't be a whole step. Then the channels stopped keeping step: each GX
stage's color ops follow the color chain and its alpha ops the alpha chain, so an alpha chain of four ops beside a color
chain of two no longer holds the next stage's color back (a color op that reads the incoming alpha still starts with
the alpha ops, which would overwrite it). Good Egg's rolling Chain Chomps, the Iron Balls and the flag planets fit
then. The disc's models went from 7,266 of 7,780 materials exact to 7,306; 14 still need more than six stages (the
Comet Luma's body takes about eight ops of color alone). `SMG_LIST_APPROXIMATED=1` makes the audit name each one.

**Alpha tests of two compares, and the water sparkle's wrap.** GX's alpha test is two compares of the alpha byte joined by
and, or, xor or xnor; the PICA has one, and `preparePixel` kept the first (`keptAlphaCompare`). The game's water sparkle
(the castle lake's `lambert118_v_x`; 19 materials of 18 planets: `(a <= 160) xor (a >= 40)`, source alpha blend) passes
a < 40 or a > 160, so the port drew exactly the other bytes: a veil of white by 4 RASA T0 T1 over the 99% of the lake
below 161, and none of the bright cores the game shows (50 to 80% of those materials' own points differed visibly; the
audit's measure ignored the alpha test, so it said 0.0%). `splitAlphaTest` finds the bytes the test passes as at most
two single compares that reach the ends of 0..255 (less than `below`, at least `from`), keeps one in the first pass
(`readPasses` copies its test into the state's material) and draws the same program again for the other
(`TevPass::Retest`: blended as the material is, under a compare of its own; the spans never overlap, so nothing blends
twice). 17 more materials need it besides the nine below: ten clamped copies of the water, six or-tests (`lambert30_v`,
`lambert15_v(2)`, PlanetDisk) and an and of two bounds; a test whose passing bytes have a gap inside them is left as it was.

Nine of the water's copies leave the last alpha unclamped: 4 P with P = RASA T0 T1 wraps modulo 256 (GX's output is
`(u8)` of the stage's 11-bit value, Dolphin's `Tev.cpp`; the alpha test and the blend see the wrapped byte). With P a byte
n the alpha is 4 n mod 256, four bands of 64 levels (two for a doubled stage). `wrapPasses` (called first by `splitTev`) draws
each band by the program of P and a ramp `4 (P - 64 band / 255)`, which is the band's alpha where n is in it, zero below and
saturated above: the compare under the byte's limit never passes there (a saturated 255 is not under 40) and a zero
blends nothing. Over the other limit the saturated ramp would pass, so that pass takes a step away at the band's end,
sharpened to one level by three stages (clamp of 4 x 8 x 8 times P - (end - 1 level)): the ramp's alpha less the step in
color, which the last stage's alpha reads from red. Six stages when P takes two (the lake's); band 0's low bytes are the
first pass (the material with its alpha saturating and a < 40 as its test), so the lake is 8 draws instead of 1. The
passes' programs are built, not translated (`TevPass::hasProgram`, `compilePass`), since GX can't read color into alpha.
The recipe is a last alpha stage `s (b c)` (s of 2 or 4; b, c from APREV, TEXA, RASA, KONST; d and a zero), a constant
color, an alpha blend of source alpha over the frame or added, and an alpha test of a prefix and a suffix. `CombinerTests`
draws each of the 256 levels of P against the TEV (all within 2/255) and 4,000 random inputs (2 to 5 off, within a level
of a band's end); with every stage held to bytes the lake's own triangles come out 0.2% off visibly with rounding, 1.1%
truncating (Citra's integer ops), against 51 to 82% before.

**What the approximated materials really are.** A material's audit reason is only its first, which can hide the real
fault. Measured against the TEV model on random byte inputs and on each model's own triangles (the interpolated vertex
color as the rasterized color, texels at the interpolated UVs; `SMG_MEASURE_APPROXIMATED=1` makes the audit do it, with
the tests' models in `tests/TevModel.hpp`), most "approximated" materials draw right: none of the
228 "unclamped output" ones differs on their real inputs (they wrap only where a vertex alpha near zero pushes a sum past
one, which the Wii would show dark too). Two real faults came out of it. The combiner's buffer was written once per
program, so a material that needed an earlier value a second time read a stale one (Kingfin's bones); a later stage may
now write it again once the earlier value's readers are done, as a write shows two stages on. And an unclamped GX sum
read only as the next stage's d, which saturates (the Starshroom's hull: vertex color plus a lerp minus a half, up to
1.5, then plus vertex color times a texture minus a half), is now kept at half scale. `planChain` finds a run of sums
into one register, each unclamped and read only as the next stage's d, the last clamped and scaled by 1/2, 1 or 2;
`chainStage` halves the first's terms, adds half of each later one, and the last applies the constant (the first d,
when fixed, folded with the biases into k, of either sign) as k / 2 and doubles. The half sum saturates only where the
whole is at least two, so it's exact where every term keeps f (2 + k) at least one; a program that overflows with
chains is compiled without them (the six-tap blur). A register read after another register took the combiner's previous output (the valve switch writes C2,
then reads C2 and the older PREV) comes from the buffer the stage that made it writes (`Arg::Saved`, `producer`):
Rosalina's hair, Bouldergeist. A constant past one minus a term is (1 - t) + (k - 1), and halved plus a term it's
t / 2 + k / 2 (Honeyhive's sky, which clipping had turned into orange clouds on black; Bowser's lava). A lerp of
three constants is one constant, which frees the slots the flag disc planets' panels needed. Chains of sums brought
Battlerock's sky, the aurora sky of Freezeflame and Snow Capsule, and the Water Bazooka capsules.

Two recipes the PICA can't follow step by step are matched by what they compute, on gray textures (I4, I8, IA4, IA8:
the renderer passes `grayMaps`, a bit per map). The game's water contour bands (`wrapBand`: WaterBack_v, Blue01_v_x,
Mizu01) wrap a product P = 2 K0 T0 T1 past a byte to light a band where 28 < P < 32 (/255); six stages build the window
from P sharpened by steps of four. OceanSphere's lines (`equalLine`) add C2 - C1 where the product's red equals A0; a
window half a byte wide around A0, sharpened by eight, lights them (off on 17 of 4,000 points against 56 without).

Passes now draw opaque materials too (`splitTev`, `TevPass::Add` and `Over`). A sum's last term, added by a blend of
one and one, saturates as GX's clamped sum does when its d is never negative (`nonNegative` follows each register to
the stage that wrote it): CrestModel's five samples, and the Pie Fortress and Dorayaki grass masks, whose stage keeps
its scale in both passes. A last stage lerping PREV to x by a scalar becomes a pass of x drawn over the rest with the
scalar as its alpha (a gray map's red read into alpha by a swap table): Cosmos Garden's sky (GreenPlanetOrbitSky). The
White Room's light streams split the same way. The audit measures a split material by drawing its passes over a random
background against GX's blend (`drawn`, `drawnInPasses`). On the way: dropping stages nothing reads (`clearDead`)
followed PREV only, so a stage writing C1 for the last stage was cleared and the grass masks compiled "exactly" as a
different material (99% off on their own triangles); it now follows every register.

A last stage can also be finished by passes over what the first drew (`finishPasses`), which the blend saturates as
GX's clamp does once: `TevPass::Sub` takes a constant away (reverse subtract), `Scale` multiplies by one below one
(the destination times the source color), `Gain` by one to two (the source times the destination, plus it). A last
stage affine in PREV, g PREV + h (its other inputs fixed), is Sub -h / g, Scale or Gain g, then Add h: the tomb
spider. PREV plus a lerp from PREV to x by t is x drawn over by t / 2, then g = 2: the Comet Luma's body. A sum with a
-1/2 bias halved, doubled or neither is drawn halved without the bias (never past one), then a quarter less and a
Gain per doubling: Freezeflame's ice rock. When the first pass leaves out a last stage whose alpha did work and no
alpha test reads it, the last pass writes the material's alpha. The models blend in full precision; the 3DS rounds to
eight bits between passes, a level or two at most. Two compiler folds helped others: a term of two constants is one
constant, and a chain's lerp between constants is a slope plus a constant (BossPlanetR50, ForceJumpPlanetA).
7,452 of 7,780 materials translate exactly and 18 more as passes, every split one 0.0% off; on their own triangles
14 still differ visibly: SunshineMario's compares (placed nowhere, 11), AirBubble's alpha (2.4%), a water band (1%)
and PlanetDisk's wrap (placed nowhere).

**Development settings.** Keys in `settings.ini` for development only, written back only when on: `dev.skip_story=1`
skips the prologue's storybook and movies; `dev.gx_dump=1` dumps a frame's draws to the log once a minute;
`dev.gx_null=1` draws nothing, to measure the game's own work; `dev.gx_timing=1` adds the renderer's parts to the perf
line (CPU-path primitives, display lists, texture copies, frame ends, then each per-call step: state builds and how many
were cache hits, the CPU path's vertices and draws, a mesh draw's lookup, uniforms and draw calls, a texture's first
use; each clock read costs Azahar 150 cycles, so it slows what it measures, by up to a fifth with all of them) with the
vertices of refused lists by reason (the perf line names new draw states whose TEV program is approximated, by reason,
without it); `dev.gx_check=1` checks each new mesh against the CPU path; `dev.gx_zones=1` shares the render thread's time out by
what it was doing (above); `dev.gx_inline=1` draws on the game's thread
instead of the render thread; `dev.start=GALAXY N [ZONE]` starts that galaxy at its start point N (StartInfo's `MarioNo`; the
named zone's, if one follows, such as Tarantox's arena, `StarDustGalaxy 100 PhantomBossSpiderZone`), to reach a stage's
later parts without walking there; it also stops the story's queued demos there, as `dev.galaxy` does
(a new file's opening would otherwise meet the garden's start point 1, the after-the-attack spawn, and the attack movie
queued on landing would freeze the opening's arrival cutscene and be frozen by it); `dev.galaxy=GALAXY N` makes the first galaxy after the file select that
galaxy's scenario N (`GameSequenceProgress::requestGalaxyMove`, once the story has named the stage: a new file's first
move names it only in `moveGalaxy`, which also queues the castle's prologue, so the story is stopped there). A hidden
star's N plays the scenario the star is placed in, with N as the one selected, as the scenario select chooses it
(`ScenarioSelectScene::trySetCurrentScenarioNo`: the first normal scenario whose `PowerStarId` bits hold the star's,
or `PlacedHiddenStarScenarioTable`'s Beach Bowl entry). `dev.warp=X Y Z [SECONDS]` moves Mario there (`MR::setPlayerPos`) once a run, after that much play (2 s unless given), counted only outside the file select (whose Mario is a
stand-in), movies and the scenario's opening; demos count, as some are played (Bowser's attack). The heartbeat
(every 300th frame) logs where Mario is, for planning a run's route. Azahar runs the 3DS CPU at its base 268 MHz although the port switches a New 3DS to 804
MHz (`osSetSpeedupEnable`), so every millisecond figure here is a slow-clock one; how much faster hardware is can only
be measured on hardware.

**Race ghosts.** A Cosmic Mario race replays a recording (`GhostData*.arc`'s `.gst`) that `GhostPacket` reads a field
at a time, copying each half and word byte by byte in the disc's order; the adapted reader puts them together
big-endian.

**More byte-order traps.** Data the GPU reads must hold the Wii's bytes even when the source wrote it as wider
integers: `MainLoopFramework`'s `clearZTexData` (Z24X8 texels as `u32`s) cleared every frame to near on ARM, and every
3D draw then failed its depth test. The SDK's GX read `GXColor` arguments through `u32` casts (TEV, konst, fog, light
and channel colors), and J3D read texture numbers back from its display lists with a native `u32` load. Region: Petari
matches a build that loads a Mii-name font (`MiiFont.arc`) the US disc doesn't have, and whose `main.dol` never
names it, so the step is skipped when the archive is absent. Checking `main.dol`'s strings settles such questions.

**Bitfields read as words.** `GXSetTevOp`'s presets (`GX_MODULATE`, `GX_REPLACE`, `GX_PASSCLR`, ...) are tables of
`tev_color_env_t` / `tev_alpha_env_t`: bitfield structs read back as register words. Metrowerks packs a struct's first
bitfield into the word's top bits, and GCC on ARM into its bottom bits. The words therefore came out scrambled, and
every `GXSetTevOp` draw ran nonsense stages. The visible case was the picture font's button icons, drawn as white
silhouettes. The adapted `GXSetTevOp` builds each word field by field by name (`smgTevColorWord`, `smgTevAlphaWord`
in `compat/smg/GxPrivate.h`); a test pins `GX_MODULATE` to the hardware's words. Reversing the field order instead
would break the SDK's positional initializers, which GCC rejects. Any other bitfield struct read as an integer has
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
constants work out to exactly) and draws it where the game's quad goes. The 3DS's Y2R converter does that and writes
the PICA's 8x8 tiles itself (a row of tiles per transfer, a gap for the texture's unused width, rows top first so
the quad's t runs the other way); `thpTiles565` is the CPU's way when Y2R won't open, texel for texel `thpPixel`'s.
The player decodes a frame every logic step, into the texture set it draws next, and the port draws every second
step: `THPVideoDecode` only keeps a copy of the frame (the wrapper says its size; the read buffer is refilled before
the draw) and `THPGXYuv2RgbDraw` decodes the last one, half the decodes. The decoder takes libjpeg's own shortcuts
for blocks and rows without AC terms, which give the same samples. A movie is paced by logic steps, so it slows with
the game: in Azahar's 268 MHz the opening's frames take ~80 ms to decode, about 11 fps. The game's `THPDraw.c` and the SDK's
`THPAudio.c` build as they are otherwise; the player's headers and each frame's size words, and each audio record's
header, turn native as they arrive. The player preloads 20 frames, each preload's callback restarting the read step; that
step read before checking how many were left, so the last callback set off a 21st read into the first frame's buffer,
which no callback then turned native. The Wii's disc drive was usually slower than the first decode; the SD card never
is, and the first frame's sizes were read big-endian. The step now reads only while frames are left to preload.

**The top screen's 5:3.** The 3DS shows the EFB's whole 640x456 at 400x240, so a screen-space unit is square at 456 lines by
760 columns (`smg::screen::Width`, `compat/smg/Screen.hpp`). The game's wide mode (`SCGetAspectRatio` answers 16:9) composes for
16:9 with 832 columns, which on the Wii's analog output (the EFB spread over 686 of the line's 720 pixels) is within a few
percent of square (832 is within 0.3% of 608 times 4/3 times 686/670, rounded to a multiple of 32); on the 3DS it left
everything 6.25% too narrow. `tools/prepare-petari.py` swaps the constants that name the shape: `MR::getScreenWidth` (832 to 760),
`CameraContext::getAspect` and `CameraViewInterpolator`'s near plane (16/9 to `Width / 456`), the layouts' location adjust
(0.75 to 608/760 = 0.8, in `LayoutManager::calcAnim`, `copyLayoutDrawInfoWithAspect` and the ISBN screen), `blurTexture`'s
vertical radius (1.333 to an EFB pixel's 760/640) and the HOME menu's adjust. Layout space is 608 columns in every mode and
the conversions between layout, screen and EFB space already divide by `getScreenWidth`, so the pointer (normalized to the
whole screen), the circle wipes, the safety frame (about 91% of the width, as in 16:9) and the star pointer's hit tests follow
without edits. A movie's 640x368 picture is a 16:9 frame's, so it keeps its width and is 345 lines tall between bars of 55.

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

All 1,758 BMD/BDL models load under ASan and UBSan (5,053,844 triangles), and 7,266 of their
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
8,810 TEV combiner, 27 original gravity, 27 NameObj, 89 scene and gravity creator, 48 collision, 21 J3D animation, 13 original actor, 6 J3D transform, 6 J3D skeleton, 14 GX command processor, 19 BMG text, 9 color, TEV and display list word, 7 ETC1 and 8 GPU texture checks on emulated ARM
in Azahar; the GPU checks compare the hardware's ETC1 decoding against ours. Of the disc's
7,780 materials, 6,987 translate to the combiner exactly. No physical console has been tested.
