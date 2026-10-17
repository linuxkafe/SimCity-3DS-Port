# REQUIREMENTS — SimCity for Nintendo 3DS

> Revised 2026-10-17. The previous version specified a Nintendo **DS** target.
> The active target is the 3DS; DS-specific requirements are kept in a separate
> section at the bottom rather than deleted, since `frontend/ds/` still exists.

## Functional — 3DS

- [x] Boot the exact SimCity (USA) ROM through static dispatch.
- [x] Render a complete frame to the top screen, rotated correctly.
- [x] Accept controller input via the hardware pad and via injection.
- [ ] Playable frame rate. **Not met** — see the measured table in README.
- [ ] Audio: PCM audible at the 3DS headphone jack. **Not started.**
- [ ] Save/load persistence (the game uses SRAM; path on 3DS not yet decided).
- [ ] Touch input for the soft pointer.

## Non-Functional — 3DS

- [x] Compiles inside the `devkitpro/devkitarm` Docker image.
- [x] Runs under Azahar with the debug bridge reachable from the host.
- [x] Reproducible build: `patches/` + `make docker-build` from a clean
      checkout, no hand-edits to the upstream core.
- [ ] **Deterministic: same ROM + same input → identical frame output,
      verified by hash.** Not met, and not yet tested at all. The colour
      conversion is proven exhaustively on the host, but stride and rotation
      are not covered by that proof.
- [x] Fail-closed: unknown execution paths abort with a diagnostic.
- [x] No dynamic recompilation, no JIT, no interpreter fallback.
- [x] Generated dispatch shards v11/v34/v35/v36 byte-identical to upstream.
- [ ] No ROM in the repository, in any branch, or in any build artefact.

## Measurement discipline

These are requirements on how results are produced, because getting them wrong
has already happened twice:

- [x] Every performance claim uses **interleaved** A/B pairs with the
      difference taken within each pair. Sequential measurement is forbidden:
      `core` drifts by up to 42% with scene content.
- [x] Contaminated pairs are **discarded**, not averaged in. A pair outside the
      accepted band is a failed observation.
- [x] Every reported figure carries its attribution state (`build`, `cclog`,
      `anlz`) — a tick count means nothing without it.
- [x] The harness reports `UNRESOLVABLE` when an effect is below the noise
      floor, rather than a number.
- [ ] A measurement harness that **fails** when a bound is exceeded, rather
      than printing a number for a human to judge.

## Constraints — 3DS

- Target: Nintendo 3DS, ARM11 at ~268 MHz, 64 MB RAM (128 MB on n3DS), top
  screen 400×240, RGB565 framebuffer.
- Toolchain: devkitPro devkitARM.
- Build: Docker (`devkitpro/devkitarm`), CMake + Makefile.
- ROM: externally supplied, legally obtained SimCity (USA).sfc (512 KB LoROM).
  Never committed — see the incident in the README's Legal section.

## Previously specified — Nintendo DS (lower priority)

The DS frontend exists but has not been developed since the target moved. Kept
here so the intent is not lost:

- [ ] Boot the ROM on DS via static dispatch.
- [ ] Render a complete frame to the DS screens (256×192).
- [ ] Touchscreen input for soft-pointer navigation.
- [ ] Persist 32 KiB battery SRAM to cartridge.
- [ ] Audio at 32 040 Hz stereo, 16-bit — flagged as possibly exceeding the
      ARM9 budget; ARM7 co-processor offload or downsampling to be
      investigated.
- [ ] Settings overlay on the touch screen.

Widescreen (398×239) is likely infeasible on the DS top screen; letterboxed
256×192 or cropped 256×160 plus a HUD would be the fallback.
