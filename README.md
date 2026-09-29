# SMG-3DS

A native Nintendo 3DS port project based on [SMGCommunity/Petari](https://github.com/SMGCommunity/Petari).

**Status: platform bring-up. The current executable does not run Super Mario Galaxy gameplay.**
It builds for ARM11 using devkitARM, libctru, Citro3D, and Citro2D. Eight Petari
source files provide the running state machine, input transition detection, field hashing,
and level-table lookup. Seven compile unchanged; JMapInfo has narrow GCC syntax adaptations
applied to generated copies outside the checkout.
The Wii renderer, Mario/actor simulation, levels, audio, saves, and game menus still need integration.

## Controls and touchscreen

| Action | 3DS input |
| --- | --- |
| Move | Circle Pad |
| Jump / swim | A or B |
| Spin / activate | X or Y |
| Crouch / ground pound | L or R |
| Aim / collect star bits | Touchscreen, shooting toggle off |
| Shoot star bits | Touchscreen, shooting toggle on |
| Recenter camera | Bottom-left touch button |
| First-person camera adjustment | D-pad |
| Pause | Start |

The complete 320×240 touch surface maps to the complete 400×240 upper screen.
The pointer appears only during an aiming touch and disappears immediately on lift.
Both corner buttons become **50% transparent during aiming**. A pointer drag can cross
either button without activating it. A fresh touch starting on a button is captured by
that button until release. The bottom-right button toggles shooting once per fresh tap;
its selected state has an amber border and an ON label. The bottom-left button is momentary.
The diagnostic build exits with Start + Select.

The current build displays the mapped inputs for testing. These mappings are not yet
connected to Mario, camera movement, star-bit collection, or shooting in the game world.
Original game icons load from the user's SD card; until imported, the development screen
explicitly uses text labels and a diagnostic reticle.

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

For portable input, archive, and original core tests:

```sh
cmake -S . -B /path/outside/checkout/host -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build /path/outside/checkout/host
ctest --test-dir /path/outside/checkout/host --output-on-failure
```

## Game assets

Petari contains no game assets. Keep your extracted disc data outside this checkout.
No copyrighted textures, models, levels, or audio are included in this project.

The runtime expects the contents of the disc's `files` directory at:

```text
sdmc:/3ds/SMG-3DS/game/
  LayoutData/
  ObjectData/
  StageData/
  ...
```

On boot, it validates `LayoutData/StarPieceCounter.arc` using the native Yaz0/RARC
reader. If `StageData/GoodEggGalaxy/GoodEggGalaxyScenario.arc` is also present, it reads
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

Repeat using kind `camera` and optionally `pointer` with their own original textures.
The importer preserves a PNG preview and source hashes next to each `.t3x` texture.
Copy `ui/star-bit.t3x`, `ui/camera.t3x`, and `ui/pointer.t3x` to the same paths under
`sdmc:/3ds/SMG-3DS/`. Actual icon selection and validation require the original game data.

## Port architecture

See [PORTING.md](docs/PORTING.md) for the engine boundaries, upstream revision,
and work that remains. A successful build or host test is not proof of real 3DS performance.
