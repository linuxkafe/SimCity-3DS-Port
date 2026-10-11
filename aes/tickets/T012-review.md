---
ticket: T012
phase: review
status: done
created: 2026-10-10
---

# T012 — Critical Review

## Simplicity Check
Three files changed, ~30 net lines added/modified. No new abstractions.
The fix is surgical: each change addresses one root cause of the symptom.

## Correctness

### Root Causes Identified and Fixed
1. **`consoleInit(GFX_TOP)`** — text console was drawn on the top screen
   framebuffer every VBlank, overwriting game pixels with ANSI escape sequences.
   **Fix:** moved to `GFX_BOTTOM`.

2. **`gfxSwapBuffers()` inside row loop** — swapping the buffer once per row
   meant the GPU presented a partially-written frame multiple times per frame.
   On double-buffered 3DS this produces a visible triplication/glitch.
   **Fix:** single `gfxFlushBuffers()` + `gfxSwapBuffers()` after all rows.

3. **`bgra_to_rgb565` bit order** — original did `(b5<<11)|(g6<<5)|r5` (B in
   high bits, R in low bits). 3DS RGB565 format is `R5 G6 B5` (R at bits 11-15,
   B at bits 0-4). This produced a blue/red swapped, near-unrecognizable image.
   **Fix:** `(r5<<11)|(g6<<5)|b5`.

4. **`gfxInitDefault()` called twice** — once in `main()`, once in
   `ds3_render_init()`. The second call could reallocate framebuffers,
   invalidating the pointers from the first call.
   **Fix:** removed from `ds3_render_init()`.

5. **citro2d include without init** — header declared `C3D_RenderTarget*` externs
   but citro2d was never initialized. This caused LSP errors and risked
   undefined behavior if anyone dereferenced those pointers.
   **Fix:** removed include and externs from header.

### Remaining Open Items
- **HUD text on bottom screen**: currently clears and redraws every frame via
  `printf("\x1b[2J\x1b[H")`. This works but is heavy — a full console clear
  every frame. Acceptable for now; consider incremental updates later.
- **Audio resampler**: pre-existing bug with static counters in both
  `ds3_audio_sink` and inline resample in `ds3_advance_frame`. Out of scope.
- **Frame timing**: main loop is button-gated (A press per frame). Not a bug
  — correct for a manual step mode — but smooth auto-play would need a timed
  loop. Out of scope.

## Technical Debt
Low. No new patterns introduced. The changes follow existing conventions from
the DS frontend (`ds_main.c`, `ds_render.c`).

## What a Teammate Needs to Know
- **Never put `consoleInit(GFX_TOP)` and direct framebuffer writes on the same
  screen.** The 3DS console draws via the same legacy framebuffer.
- **`gfxSwapBuffers()` is expensive and must be called exactly once per frame.**
  Calling it more than once per VBlank period causes visible artifacts.
- **3DS RGB565 is R5 G6 B5.** The bit positions are NOT the reverse of BGRA —
  that's the common mistake here.

## Verification Evidence
- `arm-none-eabi-gcc -c` on both changed files: exit 0, no errors
- Full build: `simcity.3dsx` linked successfully (37 MB unstripped)
- Same CMake toolchain (`devkitpro/devkitarm`) as T011 — reproducible
