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
  The bootstrap probes the original star-bit layout archive and the Good Egg scenario table
  if they are present on the SD card.
- The J3D reader loads every BMD/BDL on the disc: joint hierarchies, envelope skinning
  baked into the bind pose, per-packet matrix slots, two UV sets, all GX texture formats
  with their palettes, and the complete MAT3 material state.
- `compileTev` translates each material's TEV program into at most six PICA200 combiner
  stages and three texture units, reporting when and why it has to approximate. Host and
  on-ARM tests compare it against reference GX and PICA evaluators.
- The original `MathUtil`, `MtxUtil` and nine gravity shapes (point, parallel, cube, disk,
  disk torus, cone, segment, wire and the `PlanetGravity` base) compile for ARM from
  generated copies that include only what each file uses. Three helpers whose only bodies
  were paired-single assembly, and `fastSqrtf`/`frsqrte`, have scalar bodies; the SDK matrix
  and quaternion routines they call have portable implementations. `MR::getRandom` reads a
  natively held `TRandom_fast_` seeded as `GameSystemObjHolder` seeds it. Point, parallel,
  disk and segment gravity are under test; cube, cone, wire and torus compile but are not yet.
- The original `NameObj`, `NameObjRegister`, `NameObjHolder` and `NameObjGroup` compile for ARM,
  with `MR::getHashCode` from `HashUtil` (MSL's locale table replaced by the "C" locale's ASCII
  lowering it holds) and ObjUtil's and SystemUtil's NameObj helpers copied out of their files
  verbatim by `tools/prepare-petari.py`. Their scene is the native scene described below. Plain
  `char` is unsigned on every build, as on the Wii and the 3DS, so Shift-JIS names hash the same.
- The original `PlanetGravityManager` and `GravityInfo` combine every registered gravity by
  priority, type, host and switch state. Hosts are compared as `u32`, which holds a pointer on
  the Wii and the 3DS; 64-bit host tests compare its low half.
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
  vertices and native texture formats, and draws them with the translated combiners, a GX
  color channel with one directional light, three texgens, blending, alpha test and depth
  modes. Skinned models are skipped until animation exists. This is a stage preview with a
  free camera, not gameplay: no actor, collision or camera code from the game runs yet.

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
3. **PICA200 renderer.** The static stage path exists. Still missing: the zones' own light
   tables (a fixed light stands in), GX's second color channel, projection texgens (they
   fall back to sphere maps), mipmaps, BTK/BRK/BPK material animation, billboards, runtime
   skinning, fog, particles and postprocessing. `resource_audit --models` lists the 254
   approximated TEV programs by reason; most need more than two distinct constants (181).
   A loader thread decodes models while the main thread uploads one per frame. CMPR
   textures become ETC1/ETC1A4 (a GPU readback test confirms the block layout), leaving
   19.4 MB of linear memory after Good Egg in Azahar; other formats stay uncompressed.
4. **Original scene and actor execution.** Placed gravity needs `GravityCreator`, whose
   `MR::registerGravity` reaches `PlanetGravityManager` - a `NameObj` owned by the scene
   object holder - so `NameObj` and the scene holder come before real stage gravity.
   Then bring up the original `NameObj`/`LiveActor` scheduler,
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
  original's, copied out verbatim; `newEachObj` makes only `PlanetGravityManager` (as the game
  names it, "重力"), which the scene creates first, as `GameScene::init` does. Any other scene
  object stops the program with a message naming its id.

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

All 1,758 BMD/BDL models load under ASan and UBSan (5,053,844 triangles), and 6,953 of their
7,780 materials translate to the combiner exactly. Good Egg scenario 1 places 296 objects
across 12 zones; the preview draws 56 rigid models in 172 placements.

The portable math tests check tolerances and conventions, not bit-identical Gekko rounding.
The native validation app passes 1,069 math, 37 WPad, 179 BCSV, 1,043 J3D model/texture,
4,404 TEV combiner, 23 original gravity, 27 NameObj, 22 scene and gravity manager, 7 ETC1 and 8 GPU texture checks on emulated ARM
in Azahar; the GPU checks compare the hardware's ETC1 decoding against ours. Of the disc's
7,780 materials, 7,526 translate to the combiner exactly. No physical console has been tested.
