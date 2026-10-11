# SimCity DS Port — Quick Reference

## Build

```bash
# Inside Docker (recommended — no local devkitARM needed)
make docker-build

# Or with native devkitARM installed
make ds-build
```

## Test

```bash
# Headless frame validation (requires ROM at ~/SimCity(USA).sfc)
make ds-test-headless

# Doctor: check environment
make doctor
```

## Run on Emulator

```bash
# After docker-build, copy the .nds to your SD card
cp build/simcity.nds /path/to/melonDS/roms/

# Then run melonDS manually
melonDS /path/to/simcity.nds
```

## Controls (in-game)

| DS Button | SNES Action |
|-----------|-------------|
| D-pad     | Move / select |
| A (green) | Confirm     |
| B (red)   | Cancel      |
| X (yellow)| X           |
| Y (blue)  | Y           |
| L/R       | L/R shoulder|
| Start     | Start       |
| Select    | Select      |
| Touch     | Soft pointer|

Hotkey reset: SELECT+START+L+R

## File Layout

```
SimCityDS/
├── CMakeLists.txt          ← Main build config (cross-compiles for ARM9)
├── Makefile                ← Convenience targets (docker-build, doctor, etc.)
├── src/
│   ├── ds_port.h           ← DS-specific declarations
│   └── (core sources copied from upstream at build time)
├── frontend/ds/
│   ├── ds_main.c           ← libnds entry point
│   ├── ds_main.h
│   ├── ds_input.c/h        ← Button + touch mapping
│   ├── ds_render.c/h       ← Top/bottom screen rendering
│   ├── ds_audio.c/h        ← Audio output (placeholder, T005)
│   └── headless_test.c     ← CI/test binary
├── aes/                    ← AES protocol tracking
├── docs/
│   ├── VISION.md
│   ├── REQUIREMENTS.md
│   └── ROADMAP.md
└── build/                  ← CMake output (gitignored)
```

## Core Architecture

The port reuses the upstream static-recomp core unchanged (handwritten sources
+ generated dispatch shards). Only the frontend is DS-specific.

- Upstream core path: `/home/seyon/dev/Games/PC/scssr/SimCity-SNES-Static-Recomp/static-recomp/`
- Generated shards are read-only — never modify.
- The core produces 256×239 BGRA frames at 32,040 Hz stereo PCM.
- The DS frontend crops to 256×192 top screen and downmixes audio.

## Requirements

- Docker with `devkitpro/devkitarm` image (pulled automatically by `make setup`).
- A legally obtained SimCity (USA) SNES ROM at `~/SimCity(USA).sfc`.
- melonDS or DeSmuME emulator for testing.