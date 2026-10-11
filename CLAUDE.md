# CLAUDE.md — SimCity DS Port

## Project
Port of SimCity SNES Static Recomp 1.4.0 to Nintendo DS.
Source reference: `/home/seyon/dev/Games/PC/scssr/SimCity-SNES-Static-Recomp/`

## Environment
- Build: Docker `devkitpro/devkitarm` (ARM9 `arm-none-eabi-gcc`).
- Emulator: melonDS or DeSmuME for pre-hardware testing.
- Host: Linux, Docker available.

## Essential Commands
- `make docker-build` — build inside devkitpro container.
- `make docker-run` — run in melonDS via Docker (if configured).
- `make check` — run tests.
- `make doctor` — environment diagnostics.

## Critical Files
- `src/` — ported core (ARM11/DS adaptations of the static-recomp sources).
- `frontend/ds/` — NitroSDK DS frontend.
- `docs/VISION.md`, `docs/REQUIREMENTS.md`, `docs/ROADMAP.md`.

## Never-Do
- Do NOT modify the generated dispatch shards (v11/v34/v35/v36) — they are
  ROM-certified and must remain byte-identical to the upstream project.
- Do NOT introduce a runtime interpreter, JIT, or dynamic recompiler.
- Do NOT commit the ROM file.
- Do NOT bypass the fail-closed guard for unknown execution paths.

## Evidence Required
- Frame output must match upstream SHA-256 for the same ROM + input sequence
  (within DS resolution/geometry differences).
- Audio must be audibly correct at the DS headphone-jack output level.