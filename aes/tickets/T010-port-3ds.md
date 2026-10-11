---
ticket: T010
title: Nintendo 3DS port — frontend implementation
sprint: sprint-01
priority: high
status: in-progress
created: 2026-10-10
---

# T010 — Nintendo 3DS Port

## Context
T001 (DS port) was BLOCKED because the core exceeds the 3.5 MB DS ARM9 limit.
T010's predecessor study (docs/3DS-PORT-STUDY.md) confirmed the 3DS is viable:
125/125 source files compile for ARMv6K, core is ~7 MB vs 256 MB RAM (97% margin).

## Acceptance Criteria
- [ ] `make docker-build-3ds` produces a `.3dsx` that loads on Citra.
- [ ] Core initializes and advances frames without route failure.
- [ ] Top screen renders the SNES game frame (256×240).
- [ ] Bottom screen shows HUD/diagnostics.
- [ ] Controller input (D-pad, A/B/X/Y, L/R, Start/Select) maps correctly.
- [ ] Touch input works as soft-pointer.
- [ ] Audio is audible at DS headphone-jack equivalent level.

## Scope
**In scope:**
- `frontend/3ds/` libctru-based frontend.
- PPU rendering to both screens.
- Input mapping (gamepad + CStick + touch).
- Audio output path.
- SMDH metadata + icon.

**Out of scope:**
- Modifying generated dispatch shards.
- Cartridge SRAM persistence (T014).
- Physical cartridge .cia packaging.

## Dependencies
- T001 analysis (completed, BLOCKED → pivoted to 3DS).
- Upstream static-recomp core (unchanged).

## Rollback
Delete `frontend/3ds/`, revert CMakeLists.txt and Makefile changes.

## Notes
Full study results in `docs/3DS-PORT-STUDY.md`. Key stats:
- 125 source files, 0 compile errors
- ~7 MB core vs 256 MB 3DS RAM
- ~30-60 fps estimated (4-8× DS speed)