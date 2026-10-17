# ROADMAP — SimCity for Nintendo 3DS

> Revised 2026-10-17. The previous roadmap was for a Nintendo **DS** target and
> had not been updated since the work moved to 3DS.

The organising principle of this phase: **stop guessing where the time goes.**
Most of the remaining per-frame cost is unattributed, and the two results this
project has published that were both wrong came from optimising the phase that
looked expensive rather than the phase that was. So the next tickets are
measurement infrastructure, not optimisation.

## Phase 1 — Get to a correct baseline

| Ticket | Title | Status |
|---|---|---|
| T031 | Headless emulator environment (Azahar, Steam Deck) | **done** — guest TCP works |
| T029 | Make `analyze()` measurable | **done** — patch 0004, build ftp7 |
| T032 | Deterministic per-frame hashes | open |
| T033 | Automated paired A/B harness with bounds | **partial** — `tools/deck_ab.py` |
| — | Verify output equality on hardware (same-frame capture) | open |

T032 is the highest-value item in this phase. Without it, "did this change the
output?" is a judgement call, and T026 shipped a change whose device-side
equivalence was never established.

## Phase 2 — Attribute the remaining cost

Nothing here is scheduled. The order depends on what T032/T033 show.

| Candidate | Why it might matter |
|---|---|
| Static route loop | the bulk of the remaining ticks, unexplained |
| `smp_sync_to_master` (the DSP) | audio emulation, likely heavy |
| Composite pass | writes both 16-bit and 32-bit buffers per pixel |
| `analyze()` | 61 184 pixels through a 4096-slot hash; measured at ~0.8M under emulation, **not resolvable there** — needs hardware |

Instrument before optimising. Each candidate needs a measurement that controls
for drift before anyone argues about it.

## Phase 3 — Playable

- [ ] Playable frame rate on hardware.
- [ ] Audio audible at the headphone jack (not started; the DSP is a prime
      suspect for the frame cost, so audio and performance are one problem).
- [ ] Save/load.
- [ ] Touch input for the soft pointer.

## Phase 4 — Housekeeping

- [ ] `put` needs a recovery path. Today an interrupted transfer can only be
      escaped by killing the app: `abort` has to arrive through the same bridge
      the transfer is holding, so it is unreachable exactly when it is needed.
- [ ] `Makefile` uses `WORK_DIR ?= $(PWD)`, which breaks whenever the
      environment's `PWD` disagrees with the actual directory. Should be
      `$(CURDIR)`.
- [ ] Latent: `analyze()`'s probe loop cannot terminate if the distinct colour
      count ever exceeds 4096. Not reachable today; documented, not fixed.
- [ ] Consider whether the DS frontend in `frontend/ds/` is worth carrying.

## Known retractions

Recorded so they are not quietly forgotten:

- **−23.7% for the `bgra` table** — retracted. The change is sound and stays;
  the number was inflated by measurement drift and must not be quoted again.
- **−1.1% for the BGR565 path** — established nothing; inside the noise.
