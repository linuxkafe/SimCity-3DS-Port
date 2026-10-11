# T010 Plan — Nintendo 3DS Port of SimCity SNES Static Recomp

## Phase 1: Hostile Analysis

### Background
The DS port (T001) was BLOCKED because the static-recomp core (9 MB stripped)
exceeds the DS ARM9 hard memory limit of 3.5 MB by 2.6×. The 3DS offers
dramatically more resources.

### INSIGHTS CONSULTED
- DS linker script (`/opt/devkitpro/calico/lib/ds9.ld`)
- 3DS linker script (`/opt/devkitpro/devkitARM/arm-none-eabi/lib/3dsx.ld`)
- 3DS CMake platform (`/opt/devkitpro/cmake/Platform/Nintendo3DS.cmake`)
- libctru headers and libraries (`/opt/devkitpro/libctru/`)
- All 125 upstream source files compile for ARMv6K target with 0 errors
- Static library size: 6.95 MB (arm946e-s compiled) / 6.1 MB partial (armv6k)

### ASSUMPTIONS I'M MAKING
- [KNOWN] 3DS ARM11: quad-core @ 264 MHz, VFP4 FPU. — Justification: official hardware spec.
- [KNOWN] 3DS main RAM: 256 MB. — Justification: official hardware spec.
- [KNOWN] 3DS cartridge NAND: 128 MB (some models 256 MB). — Justification: official spec.
- [KNOWN] .3dsx homebrew format supported by Luma3DS up to ~512 MB. — Justification: Luma3DS documentation.
- [INFERRED] The core will run at ~30-60 fps on 3DS ARM11.
  → evidence chain: x86_64 runs at 60 fps; ARM11 is ~4× faster than DS ARM9;
  DS would have run at ~15 fps if it fit; 3DS should run 4-8× faster = 30-60 fps.
- [ASSUMED] libctru's 2D rendering (citro2d) can handle the frame conversion
  (BGRA→RGB565, cropping) within the per-frame budget.
  — Impact if false: need to use ctrulib's 3D engine (citro3d) for GPU-accelerated
  texture upload, adding complexity.
- [ASSUMED] 3DS audio hardware (2× 48 kHz stereo DAC channels) can play the
  32,040 Hz core output with minimal resampling.
  — Impact if false: audio timing drifts; need software resampler.

### WHAT WASN'T SPECIFIED
- Whether to target vanilla 3DS or New 3DS (New has extra 64 MB RAM + faster CPU).
- Whether widescreen (398×239) fits on the 3DS top screen (400×240 native).
- Cartridge save persistence mechanism (Nintendo Network vs local NAND).

### ALTERNATIVES I DIDN'T CHOOSE
- Option A: Keep targeting DS — rejected: hard memory limit is immovable.
- Option B: Use a traditional SNES emulator — rejected by Never-Do rule.
- Option C: Target Switch (Switchbrew) — rejected: outside project scope,
  different certification requirements.

### RISKS & SIDE EFFECTS
- The core is 7 MB; adding a full frontend (libctru, fonts, icons) may push
  the .3dsx to 10-15 MB. Still well within 128 MB cartridge limit.
- The 3DS top screen is 400×240 (3D) / 320×240 (2D). The 256×239 SNES frame
  fits with room to spare.
- Citra emulator support for .3dsx homebrew is good but not perfect; some
  timing differences may exist vs hardware.

### COST OF BEING WRONG
MEDIUM — wrong performance estimates would mean a sluggish experience, not a
broken one. The core still runs correctly even at low fps.

### SCOPE BOUNDARIES
This analysis covers hardware feasibility, toolchain availability, and memory
budget. Excludes: full frontend implementation (T010+), audio pipeline details (T013),
and emu launch testing (T016).

---

## Phase 2: Solution Proposal

### Approach
1. Create `frontend/3ds/` with a libctru-based frontend.
2. Reuse the same static-recomp core (already ported, 125/125 sources compile).
3. Render to top screen (256×240 bitmap mode) and bottom screen (touch/HUD).
4. Use citro2d for fast 2D blitting; convert core BGRA→RGB565 on the fly.
5. Audio: feed core PCM to 3DS stereo DAC via libctru audio API.
6. Input: map 3DS buttons (CStick, A/B/X/Y, L/R, Start/Select, touch) to SNES input.

### What I will NOT change
- Generated dispatch shards (unchanged, ROM-certified).
- Handwritten core sources (copy as-is, fix compile errors only).
- The upstream CMakeLists.txt.

### Verification criteria
- `make docker-build-3ds` produces a `.3dsx` file.
- The `.3dsx` launches on Citra emulator without crashes.
- After 100 frames, `simcity_recomp_instruction_count()` > 0.
- Frame output SHA-256 matches upstream for the same ROM + input sequence.

---

## Phase 3: Implementation Plan

### Files to create
```
SimCityDS/
├── CMakeLists.txt                   ← updated: add 3DS target
├── Makefile                         ← updated: add docker-build-3ds target
├── frontend/3ds/
│   ├── 3ds_main.c                   ← libctru entry point
│   ├── 3ds_main.h
│   ├── 3ds_input.c/h                ← button + CStick + touch mapping
│   ├── 3ds_render.c/h               ← top/bottom screen rendering
│   └── 3ds_audio.c/h                ← 3DS stereo audio output
├── icon.png                         ← SMDH application icon
└── docs/
    └── 3DS-PORT-STUDY.md            ← this file
```

### Build command
```bash
make docker-build-3ds
# Produces: build-3ds/simcity.3dsx
```

### Test command
```bash
make docker-test-3ds
# Runs headless frame validation inside Docker
```

### Performance estimate
- 3DS ARM11: 264 MHz quad-core vs DS ARM9: 67 MHz single-core
- FPU available (VFP4) → double-precision NTSC timing is hardware-accelerated
- Estimated frame time: 16-33 ms (30-60 fps)
- Memory usage: ~10 MB total (core 7 MB + runtime 3 MB) vs 256 MB available