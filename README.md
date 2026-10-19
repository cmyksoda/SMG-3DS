# SMG-3DS

An unfinished native port of *Super Mario Galaxy* to the New Nintendo 3DS, built on
[Petari](https://github.com/SMGCommunity/Petari), the SMGCommunity's decompilation of the game.

> **This project is archived.** Nobody is working on it, and it is not a finished or fully playable port.
> The code is left here as it was when work stopped in October 2026, for anyone who wants to study it or pick it back up.

## Status

- **The game's own code runs on the 3DS.** All 1,803 of Petari's source files compile for the 3DS's ARM11 CPU,
  running on a port of the Wii SDK layers the game calls: threads, the disc, video, the GX graphics API translated
  to the 3DS's PICA200 GPU, and the Wii's audio DSP in software.
- **In the Azahar emulator** it boots through the title screen, the file select and the storybook into the game.
  Every galaxy loads and runs without crashing, most of them at 30 fps.
- **On a real New 2DS XL** it boots and plays, but slowly: about 14 fps, mostly waiting on the GPU. The sky and some
  textures draw wrong there, and closing it from the HOME Menu ends in an error or a hang.
- **New 3DS and New 2DS only.** It needs ZL, ZR, the C-Stick and the faster CPU.
- **Only the USA disc (RMGE01) was tested.** No game data is included. The port reads your own disc image from the SD card.

## Building

```sh
git clone --recursive https://github.com/cmyksoda/SMG-3DS.git
cd SMG-3DS
tools/build-3ds.sh    # builds in Docker with a pinned devkitARM image, into ~/.cache/smg-3ds/3ds/
tools/test-host.sh    # runs the host tests under ASan and UBSan
```

The game is `mario_probe.3dsx`. It kept the name from when it was a bring-up probe.
[docs/HANDOFF.md](docs/HANDOFF.md) covers setting up the SD card, the controls, and running it in Azahar or on a console.

## Picking it up

Start with [docs/HANDOFF.md](docs/HANDOFF.md). It explains how the port is put together, how it was tested, what is
known to be broken, and what was being worked on when it stopped. [docs/PORTING.md](docs/PORTING.md) holds the
detailed notes on every part of the port.

## Made with

- [Petari](https://github.com/SMGCommunity/Petari) by the SMGCommunity, the decompilation all of this runs on (CC0)
- [devkitPro](https://devkitpro.org/): devkitARM, libctru, citro3d and citro2d
- [Azahar](https://azahar-emu.org/), the 3DS emulator, for everyday testing
- [Wiimms ISO Tools](https://wit.wiimm.de/), [Dolphin](https://dolphin-emu.org/) and [3dbrew](https://www.3dbrew.org/)
  as references for Wii discs, the Wii's hardware and the 3DS's
- AI coding assistants: OpenAI's Codex did the first bring-up, and Anthropic's Claude (in Claude Code) wrote most of
  the code after that, directed by cmyksoda and tested on real hardware

Super Mario Galaxy is © Nintendo. This is an unofficial fan project, not affiliated with or endorsed by Nintendo.
