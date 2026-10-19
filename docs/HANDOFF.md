# Picking SMG-3DS back up

This is where the port stood when work stopped in October 2026, written for whoever wants to carry it on.
[PORTING.md](PORTING.md) has the detailed notes on every subsystem, written as the work happened. This page is the map:
how the pieces fit, how to run and test them, and what was open.

## How the port works

The port doesn't rewrite the game. It runs Petari's decompiled game code nearly unchanged, on top of a 3DS
implementation of the Wii SDK that code expects.

- **Petari** is a pinned git submodule (`upstream/Petari`, commit `31bacf2`), never edited in place.
  `tools/prepare-petari.py` writes GCC-compatible copies of its sources into the build directory. These copies carry the
  fixes Metrowerks-only code, big-endian data and 64-bit hosts need, each one documented in the script.
- **Every Petari source compiles for the 3DS**: the game's 1,605 files, JSystem's 169 and nw4r's 29. The game's own
  `main()` runs.
- **The Wii SDK on the 3DS** lives in `source/compat/`. It provides threads and alarms over libctru, the MEM1/MEM2 arenas,
  disc reads, video retrace, interrupts, the Wii Remote over the 3DS's buttons, gyro and touch screen, and one loud,
  named stop for every SDK function nothing implements yet. The table in PORTING.md's "Running the game's own boot" lists
  each library and the file that implements it.
- **Graphics.** The SDK's own GX code writes its command FIFO into a portable command processor
  (`smg::GxCommandProcessor`). That processor hands each primitive to `source/compat/GxRender.cpp`, which translates TEV
  stages to PICA200 combiners and draws through citro3d. Display lists the game reuses are cached as GPU meshes. Other
  draws take the CPU path: their vertices are transformed, culled and clipped on the CPU. EFB copies (the game's
  render-to-texture) become GPU transfers. Rendering runs on its own thread on the New 3DS's third core.
- **Audio.** The Wii's DSP runs in software (`smg::WiiDsp`), speaking JAudio2's own microcode protocol. Its output
  plays through the 3DS's DSP (NDSP).
- **The disc.** `game.rvz`, `game.wbfs` or `game.iso` is read straight from the SD card, decrypted like a Wii's (see
  PORTING.md, "Disc images"). Extracted disc files work too.
- **Timing.** The game's logic is locked to 60 steps a second. The target was a stable 30 fps: two logic steps per
  drawn frame, with the frame drawing while the next steps run.

The build makes three 3DS programs:

| Program | What it is |
| --- | --- |
| `mario_probe.3dsx` | **The game.** The name dates from when it was a probe that ran the boot until something was missing. |
| `SMG-3DS.3dsx` | The earliest stage viewer, from before the game's own code ran. Kept for history. |
| `native_validation.3dsx` | The test suites, run on the 3DS's ARM CPU. Build it with `-DSMG_BUILD_VALIDATION=ON`. |

## Where it stood

**In Azahar** (which runs a New 3DS at a third of its real CPU clock, a harsh yardstick for the CPU and a forgiving one
for the GPU): the title, the file select, the storybook prologue, the Comet Observatory and every galaxy load and run
without crashing, most at 30 fps. Busy views dip into the 20s, for example the ocean in Beach Bowl Galaxy and Buoy
Base Galaxy, and Gusty Garden Galaxy's daredevil comet.

**On a real New 2DS XL** (Luma3DS, the Homebrew Launcher): it boots and plays the title, the storybook, the castle and
the Star Festival at about 70 ms a frame (around 14 fps). The GPU takes 50 to 60 ms of that. What's known to be wrong
there:

- The sky draws as blue slabs near the camera on long jumps.
- Mip levels draw wrong, so they're switched off (`dev.gx_mips=1` turns them back on).
- Closing from the HOME Menu or the power menu ends in an error or a hang.
- Water, shadows, and two thin dotted white lines in the Star Festival arrival cutscene look wrong.
- Shooting stars' trails became giant streaks. A fix is committed but was never confirmed on a console.

PORTING.md's "On a real 3DS" lists every pitfall that froze the GPU on hardware and how each was fixed. Read it before
changing the renderer: Azahar forgives a lot that a real PICA200 doesn't.

## Building

```sh
git clone --recursive https://github.com/cmyksoda/SMG-3DS.git
tools/build-3ds.sh     # 3DS programs, built in a pinned devkitARM Docker image
tools/test-host.sh     # host tests, ASan + UBSan (fatal)
```

Both scripts mount the source tree read-only and build outside it (`~/.cache/smg-3ds/`, or `SMG_BUILD_DIR` and
`SMG_HOST_BUILD_DIR`). Without Docker, a devkitPro install with devkitARM, libctru, citro3d, citro2d and zstd from the 3DS
portlibs can build it with `cmake -DCMAKE_TOOLCHAIN_FILE=tools/3ds-toolchain.cmake`. After changing anything
`prepare-petari.py` generates a header from, clean both build directories.

## Running it

Put these on the SD card (or in Azahar's `sdmc` folder):

```text
sdmc:/3ds/dspfirm.cdc              the console's DSP firmware (dump it with DSP1); Azahar accepts any file here
sdmc:/3ds/SMG-3DS/mario_probe.3dsx the game (any name works)
sdmc:/3ds/SMG-3DS/game.rvz         your disc image: game.rvz, game.wbfs or game.iso
sdmc:/3ds/SMG-3DS/settings.ini     optional, see below
```

A FAT32 card can't hold a file over 4 GB, so on a console use an RVZ or a scrubbed WBFS, not the 4.4 GB ISO.
Without `dspfirm.cdc` the game runs silent, and the bottom screen says so. Saves go to `sdmc:/3ds/SMG-3DS/nand/`.

**Azahar:** tick *Enable New 3DS mode* and leave the CPU clock at 100%. The title takes about a minute to appear. The
port's log goes to stderr: set `instant_debug_log=true` in Azahar's `qt-config.ini` and the log filter to
`*:Info Debug.Emulated:Debug`, or the log file stops a second in.

**Controls** follow the Switch release with the shoulders swapped, since the 3DS's ZL and ZR are a reach:

| Action | Button |
| --- | --- |
| Move | Circle Pad |
| Jump | A or B |
| Spin | X or Y |
| Crouch, ground pound | L |
| Shoot star bits | R |
| Camera behind Mario | ZL |
| Recenter the pointer | ZR |
| Pause / Wii minus | Start / Select |

The title's "Press both A and B" means A + R. `settings.ini` sets the pointer (`pointer=motion`, `cstick` or `stylus`),
its speeds (`motion_speed`, `stick_speed`, `invert_x`, `invert_y`) and the buttons (`bind.jump=A+B`, with the names
`jump spin crouch shoot camera recenter pause minus`). The `dev.` lines in `source/core/Settings.cpp` switch on
diagnostics, and `dev.galaxy` starts straight in a galaxy.

## How it was tested

- **Host tests** (`tools/test-host.sh`) under ASan and UBSan with recovery off. `native_validation.3dsx` runs the same
  suites on ARM in Azahar.
- **`tools/mwerks-only-asm.py`** lists Petari functions whose only body is Metrowerks inline assembly, which GCC
  silently compiles empty. **`tools/ub-scan.sh`** lists reads past the end of arrays. Both must report 0 after any new
  source joins the build.
- **A UBSan trap build of the game** (PORTING.md, "Undefined behavior at run time") ran every galaxy in Azahar. Each
  find was fixed the way the Wii computes it. For example, a divide by zero gives 0, as on the Wii's CPU (Dolphin's
  interpreter documents this). The last batch finished after work stopped: each run reached its galaxy, and one,
  Bowser Jr.'s Robot Reactor, hit a trap that was never diagnosed.
- **Azahar** was driven headlessly in Docker (Xvfb and xdotool) for timed routes, soaks, screenshots, sound capture and
  a sampling profiler over its GDB stub. Those scripts were tied to one machine and aren't in this repository.
- **Hardware test builds** reached the console over ftpd. Test builds must never write to the SD card from the render
  thread: that cost about 150 ms a frame.

## What was open, most promising first

1. **Let the GPU run while the frame is built.** citro3d's `C3D_FrameBegin` stops the GPU's command queue, and only
   `C3D_FrameEnd` starts it. So the parts of a frame split off at each EFB copy wait for the whole frame. On the
   console, the render thread waits about 51 ms for the GPU, then builds for about 23 ms while the GPU sits idle.
   [untested/gpu-overlap.diff](untested/gpu-overlap.diff) starts the GPU on each part as it's split off
   (`git apply docs/untested/gpu-overlap.diff`). It applies to the last commit and keeps Azahar at 30 fps, but it
   never ran on a console.
2. **The rest of the GPU's 50 to 60 ms.** The castle makes twelve EFB copies a frame, each with a split, a transfer and
   a pass. A test build that removes one kind of GPU work per phase and times the GPU was written but never run.
3. **Closing the app.** `aptExit` only sends PrepareToCloseApplication. libctru's own `exit` sends
   `APT_CloseApplication` last, and the port, which skips `exit` on purpose (see "On a real 3DS"), never does. That's
   the likely cause of the HOME and power-menu hangs. The candidate fix: send `APT_CloseApplication`, then wait to be
   ended.
4. **The sky.** The sky dome is a cached mesh, a huge sphere the game keeps centered on the camera. Its triangles
   cross the near plane, and the PICA's own clipping throws their corners far off the screen. The CPU path
   already clips such triangles to a guard band (`GxVertexPipeline::GuardBand`). The fix is to draw the sky's display
   lists (Sky, Air and Sun) through that path.
5. **Mip levels on hardware.** Tint each level a flat color and compare a console screenshot with Azahar's, level by
   level.
6. **Sound.** Left/right order and Surround are fixed. Loudness was never settled by ear on a console.
7. **Water, shadows and the cutscene's white lines.** These only show on hardware.
8. **The game thread's busiest scenarios** in Azahar: Luigi's Purple Coins (Toy Time Galaxy) and The Secret of Buoy
   Base. 41% of the disc's BCK animation joints never move, so caching a static joint's pose would save about 10% of
   the skeletal animation step.
9. **The Robot Reactor UBSan trap** from the last run (see above).
10. **Leftover 2-player code**: the spider webs' second-player projection, the guidance's 2P frame, the Co-Star jump.

## Design decisions

These were the project owner's calls. Anyone continuing can revisit them:

- New 3DS and New 2DS family only, with no fallbacks for the original models.
- Switch-style controls with the shoulders swapped, and a Remap page plus `bind.` lines.
- Three pointer modes: motion (the default), C-Stick and stylus. In stylus mode, the game's "Point at the screen"
  banner is removed.
- No Wii Remote strap reminder or low-battery popup. The Remote's speaker stays silent.
- No Miis anywhere in the port's game (real Mii support was a someday goal), and no Co-Star or second player.
- The top screen stays 400×240 rather than the 800-wide mode, so stereoscopic 3D stays possible later.
- A stable 30 fps was the goal, not 60.
- The Wii's common key is built in, as Dolphin has it, so WBFS and ISO images work as they are.
