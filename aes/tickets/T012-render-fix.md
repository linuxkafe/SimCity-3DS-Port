---
ticket: T012
title: 3DS top screen renders garbled text in triplicate
sprint: sprint-01
priority: high
status: done
created: 2026-10-10
---

# T012 — 3DS rendering shows imperceptible text triplicated on top screen

## Context
3DS port builds and launches on Citra. ROM loads, core advances, but top screen shows unintelligible triplicated text instead of game frame. User report: "carrega mas a única coisa que aparece é um algo imperceptível parece texto, em triplicado no ecrã de cima".

## Acceptance Criteria
- [ ] Game frame renders correctly on top screen (256×240 region)
- [ ] No console text overlay on top screen
- [ ] Colors correct (red/blue not swapped)
- [ ] Single framebuffer update per VBlank, no triplication
- [ ] HUD visible on bottom screen
- [ ] Build succeeds and headless test passes

## Scope
**In scope:** Fix rendering pipeline: console placement, framebuffer initialization, RGB565 conversion, swap logic.
**Out of scope:** Audio resampling improvements, input refactor, performance tuning.

## Dependencies
T011 complete (3DS build links). Upstream static-recomp API unchanged.

## Known Risks
Incorrect framebuffer format may corrupt memory; consoleInit on wrong screen may cause undefined VRAM layout.
