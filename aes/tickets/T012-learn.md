---
ticket: T012
phase: learn
status: done
created: 2026-10-10
---

# T012 — Diffstory

## What Changed
4 files modified, 1 header cleaned up — all in `frontend/3ds/`.

| File | Lines Changed | What |
|------|--------------|------|
| `frontend/3ds/3ds_main.c` | ~30 net removed | Console moved to GFX_BOTTOM; dead audio_sink removed; stereo deinterleaving fixed |
| `frontend/3ds/3ds_render.c` | ~20 net changed | RGB565 bit order fixed; per-row swap removed; frame centered; HUD uses console |
| `frontend/3ds/3ds_render.h` | ~5 net removed | Removed citro2d include and stale externs |
| `frontend/3ds/3ds_audio.c` | ~80 rewritten | Replaced broken CSND code with correct NDSP double-buffered playback |
| `frontend/3ds/3ds_audio.h` | ~10 net changed | Added AUDIO_RING_SIZE constant; cleaned up stale API declarations |

## What Was Untouched
- `frontend/3ds/3ds_input.c/h` — input mapping is correct, no changes needed
- `frontend/3ds/3ds_main.h` — crop constants and public API unchanged
- `CMakeLists.txt` — build configuration unchanged (NDSP is part of libctru)
- Generated shards (v11/v34/v35/v36) — ROM-certified, never touched
- `frontend/ds/` — DS frontend left as-is (DS is BLOCKED regardless)

## Why These Changes
The triplicated garbled text had three independent root causes that compounded:

1. **Console on top screen** → text framebuffer overwrote game pixels each VBlank
2. **Per-row gfxSwapBuffers** → partial buffer presented 240 times per frame instead of once
3. **Swapped RGB565 channels** → even if visible, colors were red/blue transposed making the image look like garbage text

The audio was also completely broken (stereo deinterleaved as mono, broken static counters) but that was latent — not visible until someone tried to hear anything.

## Remaining Risks
- **NDSP double-buffering assumes ring buffer never underflows**: if the core produces audio slower than 4 frames at 32040 Hz, the NDSP buffer will play silence. This is unlikely at 60 fps but worth monitoring.
- **No resampling**: we play at 32040 Hz natively. The 3DS DAC runs at the system clock divided by 512 (~44100 Hz nominal). There may be slight pitch drift. Acceptable for now; a proper resampler is a future improvement.
- **Headless test missing**: we cannot verify frame output deterministically without a headless test binary for the 3DS target. The upstream Linux headless test uses a different API (`advance_headless` + `render_current_frame`) that would need porting.
