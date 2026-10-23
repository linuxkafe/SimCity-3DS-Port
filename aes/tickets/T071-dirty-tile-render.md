# T071 — Dirty-tile render path (replaces whole-frame cache)

Status: in progress (infrastructure in place, perf optimization needs render_lines_impl changes)
Opened: 2026-10-23

## Why this ticket exists

T070 proved the whole-frame cache is the wrong architecture for the live city:
the 120×100 tilemap (VRAM 0x10200–0x15FBF) changes **every frame** (66 % of frames
in one sample). A whole-frame cache keyed on the tilemap therefore cannot hit.

The correct structure is a **dirty-tile** system:

1. **Track which tiles change**: compare the tilemap (120×100 words) against the
   previous frame, produce a 120×100 bit bitmap of dirty tiles.
2. **Re-render only dirty tiles**: each tile is 8×8 pixels. The city renderer
   already has `city_background_pixel()` which computes one pixel from the tile
   grid; invert it to render an 8×8 tile block into the framebuffer.
3. **Composite unchanged tiles from the cached framebuffer**: the previous
   frame's output is already correct for clean tiles; blit only the dirty 8×8
   blocks.
4. **Cost model**: worst case (all 12,000 tiles dirty) = current full render.
   Typical case (city simulation: ~50–200 tiles/frame) = 100–400× less work.
   30 fps needs ≤ 0.44 s render; 200 tiles × 64 pixels × ~20 cycles = 0.25 s.

## Architecture

```
frame N:
  1. city_tile_grid_build()        // unchanged, builds 120×100 entry grid
  2. dirty_bitmap = tilemap_diff(prev_tilemap, curr_tilemap)
  3. for each dirty tile (tx, ty):
         render_tile_8x8(tx, ty)   // new function, uses existing per-pixel path
  4. for each clean tile:
         blit_from_cached(tx, ty)  // memcpy 8×8 from prev_framebuffer
  5. swap framebuffer pointers
```

The tilemap lives in WRAM at 0x10200 (24,000 bytes). A `memcmp` + bit-pack is
trivial (~3000 cycles). The existing `city_background_pixel` already resolves
tile → pixel; `render_tile_8x8` just loops 8×8 over it.

## Acceptance criteria

- [ ] Byte-identical to full render on deterministic host run (headless-profile
      --hash --shots, identical to current render_current_frame).
- [ ] Live city (T048 cadence): host fps ≥ 30, `core` ≤ 1.05 s.
- [ ] Scrolling city: fps recorded honestly; dirty count reported.
- [ ] Bridge verb `dirty` to toggle on/off in same session.

## Measurement Results (2026-10-23)

Gate: PASS (byte-identical, 200 frames, scrolling input)
Performance: ~13 fps (unchanged from baseline)

Root cause of no improvement: the dirty-tile overlay runs AFTER the full render.
The full render (sc_v28_render_first_visible_frame) still evaluates all 12,000
city tiles every frame. The overlay only re-paints dirty tiles on top, which
is redundant (the full render already produced the correct output).

To actually reduce render cost, the full render must SKIP city tile evaluation
(BG1/BG2) for clean tiles. This requires:
1. Modifying render_lines_impl to accept a skip_city_tiles flag
2. When set, skip city_tile_grid_build and city_background_pixel for clean tiles
3. The dirty tile overlay then renders only dirty tiles

The rskip=SC_RENDER_SKIP_BG mechanism was tested but doesn't work well in
widescreen mode (g_bg_prebuf filling is not gated by rskip).

## Next Steps

1. Add a skip_city_layers parameter to sc_v28_render_first_visible_frame
2. When set, skip city_tile_grid_build and city_background_pixel
3. The dirty tile overlay renders only dirty tiles
4. Clean city tiles remain from the previous framebuffer (need prev_bgra buffer)

## Non-claims

- This ticket does not touch sprites, windowing, mosaic, colour-math,
  brightness, or widescreen — those remain full-frame for now. The dirty path
  is for BG1/BG2 (the city map) only.
- Does not depend on T069 (native GPU) or T066 Phase 3 (native blocks).