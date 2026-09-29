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
  `TriggerChecker`, and JGadget `hashcode` translation units compile for ARM11 without editing their sources.
- Original `JMapInfo` compiles through generated GCC-compatible source/header copies. These fix
  switch/goto initialization scopes, inline definitions, and unnecessarily broad SDK includes.
  `Bcsv` validates and converts raw Wii records into aligned native-endian storage before
  attaching that storage to the original reader. Tests exercise its actual lookup functions.
- `compat/revolution/types.h` supplies fixed-width types without redefining C++ keywords.
- `Input` samples logical actions through Petari's `TriggerChecker`; `toWiiInput`
  translates actions to the original Wii button bitmasks and explicit shake state.
- Two native render targets display the touchscreen interface and a platform diagnostic.
- Stylus gesture ownership prevents UI presses from generating shots, and allows
  aiming across 50%-transparent buttons. Touch coordinates cover the entire upper screen.
- Yaz0 decompression and RARC indexing work independently of host endianness and alignment.
  The bootstrap probes the original star-bit layout archive and the Good Egg scenario table
  if they are present on the SD card.

The `WiiInput` result is an integration boundary; it is not yet wired into `MR::getWPad`.
No silent Wii SDK stubs are used to make gameplay appear to work.

## Next engine work

1. **Portable math and SDK surface.** Audit every Metrowerks-only function before using it.
   For example, `JMAFastSqrt`, `JMAHermiteInterpolation`, and `gekko_ps_copy*` in
   `JSystem/JMath/JMath.hpp` contain missing non-Metrowerks implementations; merely compiling
   them with GCC would introduce wrong results. Replace PowerPC paired-single math with
   tested scalar/VFP implementations. Preserve original coordinate and matrix conventions.
2. **Endianness and resource loading.** Connect SD-backed archives to JKR resource lookup.
   Validate the BCSV/JMap adapter on actual game data, then adapt BMD/BDL, animation,
   collision, effects, layouts, and audio readers;
   direct casts of Wii bytes to ARM structures are insufficient. Handle the game's Shift-JIS
   names and text encodings deliberately.
3. **PICA200 renderer.** Establish a static original BMD scene with correct indexed attributes,
   transforms, depth, and textures. Translate J3D materials/TEV state, skinning, display lists,
   camera projection, particles, and postprocessing. Measure conversion/storage tradeoffs
   with real game scenes before choosing simplifications.
4. **Original scene and actor execution.** Bring up the original `NameObj`/`LiveActor` scheduler,
   gravity, collision, stage placement, Mario, animation, and camera using the real resources.
   Integrate the input boundary into `GamePadUtil`/`WPad`, including explicit digital spin input.
   Skip the Wii pointer's multi-frame loss-of-IR delay: stylus lift must hide it immediately.
5. **Complete gameplay.** Stage transitions, menus, save migration/format, audio through NDSP,
   cutscenes, effects, motion-controlled minigames, and all game-specific exceptions.
6. **Hardware validation and optimization.** Frame time, draw count, texture/material limits,
   heap peaks, streaming stalls, suspend/resume, and input accuracy on a real 3DS.

Initial performance work will assume New 3DS unless the hardware target is clarified.
The bootstrap uses ARMv6K/libctru APIs and has no New-3DS-only controls. This does not establish
that full Galaxy gameplay can fit or run acceptably on either model.

## Pointer details to preserve

`StarPieceShooter::tryShoot` consumes both Wii B hold and trigger states. The touchscreen
adapter exposes a trigger on the first aiming sample in shooting mode and hold while the
stylus remains down; the original game should retain ownership of shot timing and charge.
The mode-toggle tap itself never emits B. Camera presses map to Nunchuk C, jump aliases to
Wii A, crouch aliases to Nunchuk Z, and the D-pad preserves its four original bits.

Aliased physical buttons are combined before transition detection, so pressing B while
holding A does not create a second jump edge, and releasing one does not release the other.

## Required validation

- Exact top-screen corner mapping, lift-to-hide, both button aliases per action, and diagonal
  Circle Pad magnitude. Repeat these on hardware because touch calibration is device-specific.
- Toggle taps, long holds, drags onto/off either button, half-opacity feedback, camera in shooting
  mode, resume during a touch, and a new aim after resume.
- Real original icon provenance and visual readability at 3DS resolution.
- Original Mario movement and planet transitions before calling any build a gameplay port.
- Boot and frame-time evidence on hardware before making performance claims.
