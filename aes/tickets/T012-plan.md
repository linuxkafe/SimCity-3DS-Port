---
ticket: T012
phase: plan
status: done
created: 2026-10-10
tier: standard
requires:
  - aes/kanban.md
  - aes/tickets/T012-render-fix.md
produces:
  - aes/tickets/T012-plan.md
---

# T012 — Plan

## Reconnaissance Summary
3DS build links (T011 complete). Frontend uses libctru legacy framebuffer. Code currently:
- `3ds_main.c` calls `consoleInit(GFX_TOP, NULL)` **before** `ds3_render_init()`
- `3ds_render.c` calls `gfxInitDefault()` again in init, claims framebuffers, copies BGRA→RGB565 with wrong bit ordering.
- Header imports citro2d.h but code never uses citro2d, leaving externs undeclared.
- `ds3_render_frame` calls `gfxFlushBuffers(); gfxSwapBuffers();` **on every row**, causing triple presentation and “triplicado”.
- `bgra_to_rgb565` does `(b5<<11)|(g6<<5)|r5` — 3DS RGB565 is R at bits 11-15, G at 5-10, B at 0-4. Bit positions swapped.
- Core frame is 256×239; 3DS top is 400×240 (3D) or 400×240 native. Currently writing 256×240 starting at 0,0 with no scaling.

## Hostile Analysis
ASSUMPTIONS I'M MAKING:
- [KNOWN] libctru legacy framebuffer mode returns RGB565 pointer via gfxGetFramebuffer — justification: libctru gfx.h docs.
- [KNOWN] consoleInit places text console on given screen, overwriting framebuffer content — justification: libctru console.h.
- [INFERRED] Triplication caused by gfxSwapBuffers called per row and console refresh on VBlank — evidence: console draws each VBlank, swap flushes partially written buffer.
- [ASSUMED] Core outputs BGRA little-endian with blue in bits 0-7, green 8-15, red 16-23 — justification: upstream Linux frontend test_color_ground_truth.c.
- [UNKNOWN] Exact 3DS top screen resolution when using GFX_TOP with citro2d disabled — outside reliable knowledge without hardware test.

WHAT WAS NOT SPECIFIED:
- Whether top screen should be 400×240 scaled or 256×240 centred.
- Whether console output needed at all.

ALTERNATIVES NOT CHOSEN:
- Switch to citro2d full pipeline — rejected: more invasive, need shaders, not needed for static bitmap blit.
- Keep console on top screen — rejected: conflicts with rendering.

RISKS AND SIDE EFFECTS:
- Changing console screen may move debug output; acceptable.
- Wrong RGB565 mapping corrupts colors permanently.

COST OF BEING WRONG: medium — visual corruption blocks usability, but no data loss.

SCOPE BOUNDARY:
In scope: fix render init order, console placement, RGB565 conversion, swap logic. Out of scope: audio, input mapping.

## Technical Approach
1. Move console to bottom screen only.
2. Init gfx once in main, not again in render init; remove gfxInitDefault from render.
3. Fix RGB565 conversion: r5<<11 | g6<<5 | b5.
4. Render whole frame then single gfxSwapBuffers per VBlank, not per row.
5. Center 256×239 frame in 400×240 top screen with black borders.
6. Remove citro2d include to avoid compile errors, or provide stub.

## Affected Files
| File | Operation | Description |
|------|-----------|-------------|
| frontend/3ds/3ds_main.c | modify | Move consoleInit to GFX_BOTTOM, remove duplicate gfxInit, call swap once per frame |
| frontend/3ds/3ds_render.c | modify | Fix RGB565 conversion, fix framebuffer sizing, remove per-row swap |
| frontend/3ds/3ds_render.h | modify | Remove citro2d dependency, clean externs |

## Verification Criteria
- [ ] Top screen shows recognizable SimCity frame, not text
- [ ] No triplicated image
- [ ] Colors appear correct (green map, blue water)
- [ ] Build succeeds: `make docker-build PLATFORM=3DS`
- [ ] Headless test passes 300 frames

## Estimation
- Complexity: medium (2–4h)
- Risk: medium
