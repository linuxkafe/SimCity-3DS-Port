---
ticket: T001
title: Port static-recomp core to ARM11/DS-Nitro
sprint: sprint-01
priority: high
status: in-progress
created: 2026-10-10
---

# T001 — Port static-recomp core to ARM11/DS-Nitro

## Context
The upstream static-recomp core (`static-recomp/`) is x86_64 C11/C++17 with
generated dispatch shards. To run on Nintendo DS we must:
- Rebuild the core under `arm-none-eabi-gcc` (ARM9, `-mcpu=arm946e`).
- Adapt memory layout to DS constraints (IWRAM 128 KB, BWRAM 256 KB, VRAM).
- Keep the generated dispatch shards byte-identical to upstream.
- Build inside the devkitpro Docker image.

## Acceptance Criteria
- [ ] Core compiles under `arm-none-eabi-gcc` with `-mcpu=arm946e -mthumb`.
- [ ] Generated dispatch shards are NOT modified (checksum against upstream).
- [ ] `simcity_recomp_create()` succeeds with the exact USA ROM.
- [ ] `simcity_recomp_advance()` produces at least one frame without route failure.
- [ ] Build artefact is a `.nds` ROM or a `.elf` loadable by melonDS.

## Scope
**In scope:**
- Copying and adapting the handwritten core sources (src/*.c) to DS.
- Creating a DS CMakeLists.txt / Makefile build.
- Creating a minimal DS frontend that can advance one frame and dump it.

**Out of scope:**
- Modifying generated dispatch shards.
- Full audio pipeline (T005).
- Touchscreen settings overlay (T007).
- Cartridge SRAM persistence (T004).

## Dependencies
- Docker with devkitpro/devkitarm image.
- Exact SimCity (USA) ROM for integration testing.

## Rollback
Delete `src/`, `frontend/ds/`, `build/`. The DS port is additive; the upstream
project is untouched.

## Known Risks
- ARM9 has no FPU; the core uses `double` for timing — must be replaced with
  fixed-point or integer rational arithmetic.
- `SCV11Runtime` is ~50 KB of state; DS IWRAM is 128 KB — layout must be
  carefully partitioned.
- The generated dispatch uses `uint64_t` counters; ARM9 is 32-bit — must
  verify no 64-bit multiply/divide assumptions.

## Notes
This ticket is the foundation. Everything else depends on it.