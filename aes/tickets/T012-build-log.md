---
ticket: T012
phase: build
status: done
created: 2026-10-10
---

# T012 — Build Verification Log

## Summary
3DS build compiles and links successfully after rendering + audio pipeline fixes.
`build/simcity.3dsx` produced (37 MB unstripped).

## Changes Made

### Rendering Fixes
- **frontend/3ds/3ds_main.c**: Moved `consoleInit` from `GFX_TOP` to `GFX_BOTTOM`.
  Placing console on top screen overwrote the game framebuffer with ANSI
  escape sequences each VBlank, producing the triplicated garbled text.
- **frontend/3ds/3ds_render.c**:
  - Fixed `bgra_to_rgb565`: was `(b5<<11)|(g6<<5)|r5`, now `(r5<<11)|(g6<<5)|b5`
    (3DS RGB565 format: R5 at bits 11-15, G6 at 5-10, B5 at 0-4).
  - Removed duplicate `gfxInitDefault()` call from `ds3_render_init()` — already
    called once in `main()`.
  - Removed per-row `gfxSwapBuffers()` call. Swapping inside the row loop caused
    triple buffering / triplicated display because each swap presented the
    partially-written buffer. Now swap once after all rows are written.
  - Added black border padding to center the 256-wide frame in the 400-wide
    top screen.
  - Replaced TODO HUD stub with console-based diagnostic output on bottom screen.
- **frontend/3ds/3ds_render.h**: Removed `#include <citro2d.h>` and stale
  `C3D_RenderTarget` extern declarations — citro2d was never initialized or used.

### Audio Fixes
- **frontend/3ds/3ds_audio.c**: Replaced broken CSND-based audio with correct
  NDSP double-buffered playback at 32040 Hz (core's native rate).
  - Was: `csndPlaySound` with wrong flags, static resampler counters, mono-split bug
  - Now: `ndspChnWaveBufAdd` double-buffering, no resampling needed
- **frontend/3ds/3ds_main.c**: Removed dead `ds3_audio_sink()` callback.
  Fixed audio deinterleaving: chunk is now read as interleaved stereo (L,R,L,R)
  instead of being split into mono channels incorrectly.
- **frontend/3ds/3ds_audio.h**: Added `AUDIO_RING_SIZE` constant; removed stale
  `ds3_audio_push()` and `ds3_audio_drain()` declarations.

## Build Output
```
Linking CXX executable simcity.3dsx
Built target simcity-3ds
simcity.3dsx: 37 MB (unstripped)
EXIT: 0
```

## Compilation Check
```
frontend/3ds/3ds_main.c   → exit 0
frontend/3ds/3ds_render.c → exit 0
frontend/3ds/3ds_audio.c  → exit 0
Only warnings: libctru zero-size array extensions (from headers, not our code)
```

## Remaining Open Items
- **Headless test**: Not yet run for 3DS platform. The upstream
  `headless-test.c` uses `simcity_recomp_advance_headless()` +
  `simcity_recomp_render_current_frame()` — a cleaner API than the current
  `simcity_recomp_advance()` + direct `frame_bgra()` path. Worth migrating.
- **Frame timing**: Current loop is button-gated (A press per frame). Correct
  for manual step mode, but smooth auto-play would need a timed loop.
- **SMDH metadata**: Application icon and title not yet added to the .3dsx.
