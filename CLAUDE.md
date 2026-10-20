# CLAUDE.md — SimCity 3DS Port Operational Contract

> This file is the operational contract for this project. It is read by the AES
> system on every task. Keep it under 200 lines.

## Project Identity

- **Name**: SimCity 3DS Port
- **Goal**: Native SimCity (USA) SNES ROM on 3DS hardware via static recompilation
- **Target**: Nintendo 3DS (ARM11 @ 268 MHz, 64/128 MB RAM)
- **Language**: C (C99), devkitARM toolchain
- **Build**: Docker (`devkitpro/devkitarm`) + CMake

## Critical Files

| File | Purpose |
|------|---------|
| `Makefile` | Build orchestration (Docker + native) |
| `frontend/3ds/3ds_main.c` | 3DS entry point, frame loop |
| `frontend/3ds/3ds_render.c` | Framebuffer, rotation, colour conversion |
| `frontend/3ds/3ds_input.c` | Hardware pad + injected input |
| `frontend/3ds/3ds_audio.c` | PCM audio to headphone jack |
| `patches/` | Patches applied to upstream static-recomp core |
| `tools/three.py` | Automated build/upload/restart/debug on Steam Deck |
| `tools/deck_ab.py` | Paired A/B measurement harness |

## Essential Commands

| Command | Purpose |
|---------|---------|
| `make docker-build` | Build 3DS in Docker (default) |
| `make 3ds-build` | Native build (requires devkitARM) |
| `make 3ds-test-headless` | Headless test under Azahar |
| `make doctor` | Environment health check |
| `make three` | Full pipeline: build → upload → restart → debug |
| `make check` | Run all quality gates (lint, format, tests) |

## Non-Goals

- **No DS target** — `frontend/ds/` exists but is not maintained
- **No dynamic recompilation** — static dispatch only, fail-closed
- **No ROM in repo** — legally obtained ROM supplied externally
- **No interpreter/JIT fallback** — unknown paths abort with diagnostic
- **No optimisation without measurement** — measurement discipline enforced

## Never Do

- Edit upstream core directly — use `patches/` only
- Commit ROM or any derivative
- Run 3DS emulator (Azahar/Citra) on this host — Steam Deck only
- Quote retracted performance numbers (−23.7% bgra table, −1.1% BGR565)
- Optimise without controlling for `core` drift (up to 42% variance)
- Average contaminated A/B pairs — discard them

## Evidence Required

- Every performance claim: interleaved A/B pairs, attribution state (`build`/`cclog`/`anlz`)
- Frame output equality: per-frame hash verification
- Build reproducibility: clean checkout + `patches/` + `make docker-build`

## AES Protocol

- Tier: **AES-standard** (3+ files, architectural decisions, measurement discipline)
- Bootstrap: Run `make aes-bootstrap` to install hooks
- Tickets: `aes/tickets/TXXX-name.md`
- Diffstory: Required in every build output
- Review: Hostile senior engineer lens (correctness, simplicity, maintainability, security, performance)

## Key Constraints (from REQUIREMENTS.md)

- Deterministic: same ROM + same input → identical frame output (hash-verified)
- Fail-closed: unknown execution paths abort
- Generated dispatch shards v11/v34/v35/v36 byte-identical to upstream
- Measurement: interleaved A/B, discard contaminated, attribution state required

## Measurement Discipline (Binding)

1. **Interleaved A/B only** — sequential measurement is forbidden
2. **Discard contaminated pairs** — do not average them in
3. **Attribution state mandatory** — every figure carries `build`/`cclog`/`anlz`
4. **UNRESOLVABLE** — report when effect below noise floor, not a number
5. **Harness must fail on bound exceedance** — not print for human judgement