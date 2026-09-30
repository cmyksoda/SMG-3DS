# SMG-3DS

A native Nintendo 3DS port project based on [SMGCommunity/Petari](https://github.com/SMGCommunity/Petari).

**Status: renderer bring-up. The current executable previews Galaxy stages; it does not run gameplay.**
It builds for ARM11 using devkitARM, libctru, Citro3D, and Citro2D. Eleven Petari
source files provide the running state machine, original button/stick handling, field hashing,
level-table lookup, and trigonometric tables. Nine compile unchanged; JMapInfo and the
trigonometric table source have adaptations applied to generated copies outside the checkout.
Mario/actor simulation, collision, animation, audio, saves, and game menus still need integration.
The target is the New Nintendo 3DS and New 2DS family.

## Controls

The layout follows the Switch release of Super Mario Galaxy, so anyone who played it in
3D All-Stars or the Galaxy 1+2 collection already knows it. SMG-3DS targets the New
Nintendo 3DS and New 2DS family; the original models lack ZL, ZR and the C-Stick.

| Action | New 3DS |
| --- | --- |
| Move | Circle Pad |
| Camera | C-Stick, unless it drives the pointer |
| Jump / swim | A or B |
| Spin | X or Y |
| Crouch / ground pound | ZL |
| Shoot star bits | ZR |
| Reset camera | L |
| Recenter the pointer | R |
| First-person view and camera nudges | D-pad |
| Pause | Start |

The star pointer has three modes, chosen on the bottom screen's **Options** page:

- **Motion** (default) — tilt and turn the console, like handheld mode in the Switch
  release. Sideways movement follows turns about the real vertical, measured against
  gravity, so it works at any holding angle. A resting gyro offset is learned while the
  console is still. R recenters.
- **C-Stick** — the nub moves the pointer, with fine control near the center. R recenters.
- **Stylus** — touching the bottom screen aims at the matching spot on the top screen,
  and the pointer disappears the moment the stylus lifts. ZR shoots; the bottom-right
  button toggles touch-to-shoot. A touch that starts on a button belongs to that button
  until release, and a drag can cross any button without pressing it. The corner buttons
  turn 50% transparent while aiming.

Options also sets motion and C-Stick speed, inverts either axis, and restores the Switch
layout. Settings are saved to `sdmc:/3ds/SMG-3DS/settings.ini`; buttons are remapped by
editing its `bind.` lines, for example `bind.shoot=ZR+R`. The bottom screen's corner
buttons reset the camera and recenter the pointer, and it lists the current bindings.

The current build streams in Good Egg's first scenario as a stage preview: every rigid model
its layers place, drawn with the game's own materials translated to the 3DS combiner. Fly
with the Circle Pad, look with the C-Stick or D-pad, rise with A, sink with ZL, hold X to
go faster, and press L to return to the start view. None of these inputs reach Mario, the
camera, or star bits in a running stage yet.
Original game icons load from the SD card once imported; until then the screens use text
labels and a plain reticle. The diagnostic build exits with Start + Select.

## Build

Initialize the pinned upstream checkout:

```sh
git submodule update --init
tools/build-3ds.sh
```

The build script uses a pinned devkitPro Docker image and writes to
`~/.cache/smg-3ds/3ds/SMG-3DS.3dsx`. Set `SMG_BUILD_DIR` to choose another **external** build directory.
The source tree is mounted read-only. A normal devkitPro installation can instead build with:

```sh
cmake -S . -B /path/outside/checkout/3ds -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=tools/3ds-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build /path/outside/checkout/3ds
```

For portable math, input, archive, model, and original core tests:

```sh
cmake -S . -B /path/outside/checkout/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /path/outside/checkout/host
ctest --test-dir /path/outside/checkout/host --output-on-failure
```

Configure the 3DS build with `-DSMG_BUILD_VALIDATION=ON` to also build
`native_validation.3dsx`. This separate app runs the math, original controller, BCSV, and model
tests on ARM and displays their result; it exits with Start.

## Game assets

Petari contains no game assets. Keep your extracted disc data outside this checkout.
No copyrighted textures, models, levels, or audio are included in this project.

For an ISO, [Wiimms ISO Tools](https://wit.wiimm.de/wit/cmd-extract.html) can extract
the data partition into a new external directory:

```sh
wit extract /path/to/galaxy.iso /path/to/extracted --psel data --pmode none
```

The resulting `files` directory is the game-data root. The host `resource_audit`
tool checks its RARC archives and `.bcsv` files; extensionless stage tables are not
included in that audit. Currently it reports the four U8-format
Wii HOME Menu archives as unsupported.

The runtime expects the contents of the disc's `files` directory at:

```text
sdmc:/3ds/SMG-3DS/game/
  LayoutData/
  ObjectData/
  StageData/
  ...
```

On boot, it validates `LayoutData/StarPieceCounter.arc` using the native Yaz0/RARC
reader. If `StageData/EggStarGalaxy/EggStarGalaxyScenario.arc` is also present, it reads
`ScenarioData.bcsv` through the original JMapInfo and reports the record count.
These are resource probes, not a full-game data validation or playable stage.
The readers decode big-endian fields explicitly and check bounds before use.

Build `archive_tool` with the host build above to list archive contents:

```sh
/path/to/host/archive_tool /path/to/game/LayoutData/StarPieceCounter.arc
/path/to/host/archive_tool /path/to/game/LayoutData/CameraInfo.arc
```

Select the actual star-bit and camera textures from those listings after visual inspection.
Import each exact texture with `tools/import-icon.py`; do not guess an icon's provenance.
The importer requires Python 3.11+, Wiimms Image Tool (`wimgt`), and Docker:

```sh
python3 tools/import-icon.py star-bit /path/to/archive.arc exact/texture.tpl \
  --archive-tool /path/to/host/archive_tool --output /path/to/sd-staging/3ds/SMG-3DS/ui
```

For the USA disc, the visually verified selections are:

| Kind | Archive in LayoutData | Entry |
| --- | --- | --- |
| `star-bit` | `StarPieceCounter.arc` | `timg/myiconstarpiece.tpl` |
| `camera` | `CameraInfo.arc` | `timg/myiconcamera.tpl` |
| `pointer` | `DPDPointer.arc` | `timg/dpdpointer.tpl` |

The pointer renderer mirrors the 16×32 half-star horizontally, matching the original
layout's texture addressing, and tints it blue or amber for the selected touch mode.
The importer preserves a PNG preview and source hashes next to each `.t3x` texture.
Copy `ui/star-bit.t3x`, `ui/camera.t3x`, and `ui/pointer.t3x` to the same paths under
`sdmc:/3ds/SMG-3DS/`. Original textures must come from your game data.

## Port architecture

See [PORTING.md](docs/PORTING.md) for the engine boundaries, upstream revision,
and work that remains. A successful build or host test is not proof of real 3DS performance.
