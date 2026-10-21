---
ticket: T046
title: Performance baseline under the headless emulator — and a contradiction it exposes in T023
sprint: sprint-02
priority: high
status: in-progress
depends_on: T041
created: 2026-10-21
---

# T046 — Performance under the headless emulator

## The measurement

All numbers below are measured, on the Steam Deck, with the headless Azahar
build (T041) and the port build from `129cce7`.

`g_dbg_phase_us[]` holds **microseconds** — `ds3_ticks_to_us()` converts at
the point of measurement (`frontend/3ds/3ds_main.c:199-201`), so the name is
accurate on this build. T023's unit reading was right.

### Throughput

| Quantity | Emulator | T023 hardware | Ratio |
|---|---:|---:|---:|
| SNES frames / wall second | **11.0** (660 frames / 60 s) | 1.59 | 6.9× |
| guest instructions / wall second | **125,083** | 19,476 | 6.4× |
| guest instructions / frame | **11,764** | 12,281 | 0.96× |

The work per frame is the same. The emulator is simply faster at doing it.

### Per-frame phase profile (microseconds, and % of loop)

| Phase | Emulator | % | T023 hardware | % |
|---|---:|---:|---:|---:|
| adv | 3,434,993 | 89.1% | 3,467,662 | 94.4% |
| core | 3,374,330 | **87.5%** | 3,407,572 | 92.7% |
| render | 60,550 | 1.6% | 59,981 | 1.6% |
| hud | 7 | 0.0% | 1 | 0.0% |
| audio | 548 | 0.01% | 13 | 0.0% |
| dbg | 338,319 | 8.8% | 93,558 | 2.5% |
| present | 82,425 | 2.1% | 82,432 | 2.2% |
| **loop** | **3,856,391** | 100% | **3,674,675** | 100% |

`core` within 1%, `render` within 1%, `present` identical to four significant
figures. **The emulator reproduces the hardware's per-phase costs almost
exactly.** That is the strongest available evidence that emulator-based
profiling is measuring the same thing T023 measured.

### The debug bridge costs nothing (A/B)

Built the port with `-DDS3_NET_BRIDGE=0` (the whole TCP + FTP surface
compiled out) and ran both for 60 s:

```
bridge-on   Ran 217756 slices   reached frame 660
bridge-off  Ran 198117 slices   reached frame 660
```

**Identical frame count.** The `dbg` phase's 8.8% share under the emulator is
not guest work — it is the emulator's IPC round-trip latency, charged to the
guest's clock while it waits for the soc service to answer. On hardware the
same `accept()` returns EAGAIN immediately, and T023 measured it at 2.5%.

So: the bridge is free, and it can stay on during measurement runs. T023's
suggestion that removing it would "recover ~2.5%" is right for hardware and
wrong for the emulator, where it recovers nothing.

### The emulator's own cost

- 99% of one core of the Deck's 8. It is CPU-bound on the guest.
- 217,756 emulation slices per wall second.
- The guest consumes **42.4 s of emulated 3DS-clock time per wall second**
  (3,856,391 µs of guest time per frame × 660 frames / 60 s). The 3DS's
  ARM11 ticks at 26.83 MHz, so the emulator is running the guest's clock at
  ~42× real time.

## The contradiction this exposes

The guest needs **3.86 s of 3DS-clock time per SNES frame**. That is the
portable number, and it is the same on hardware and in the emulator (T023's
hardware `loop` was 3.675 s).

On hardware, 3DS-clock time *is* wall-clock time. So one SNES frame should
take ~3.7 s and the frame rate should be **0.27 fps**.

**T023's wall-clock measurement says 1.59 fps** — "12.6 s wall clock,
20 emulated frames". Those two numbers differ by 5.9× and cannot both be
right.

The emulator makes the discrepancy unambiguous, because it separates the two
clocks:

- guest clock: 3.86 s of emulated time per frame
- wall clock: 91 ms per frame

On the emulator that is a legitimate 42× gap. On hardware the gap must be
1×. T023's wall-clock number says the gap was ~6×, and the emulator cannot
reproduce a 6× gap of that kind — it has a 42× gap.

**So either T023's 1.59 fps wall-clock measurement is wrong, or its
phase-derived 0.27 fps is wrong, or the 3DS's `svcGetSystemTick()` does not
advance at 26.83 MHz in real time under CPU saturation.**

That last possibility is the one that would explain both, and it is exactly
the confusion the port's own comment warns about — but it has to be settled
on hardware, not reasoned about.

## Why this matters

T023's headline conclusion is "**core is the bottleneck — 92.7% of the
loop**". If the guest's 3.86 s of clock time per frame is not wall-clock
time, then 92.7% is a share of the *guest's* clock, and the wall-clock
share of the core could be very different. T025 is "core perf deep-dive" —
optimising the core is the right target if the core really is 92.7% of the
frame, and possibly the wrong target if it is not.

**This must be settled before T025 starts optimising.**

## Acceptance

- [x] Throughput measured under the headless emulator
- [x] Per-phase profile measured and compared with T023's
- [x] The debug bridge's cost measured by A/B (bridge on vs compiled out)
- [x] The emulator's own cost measured
- [ ] The contradiction with T023's 1.59 fps is settled **on hardware**
- [ ] T023's baseline is corrected or confirmed, and the correction recorded
- [ ] T025's target is chosen against the corrected baseline

## Notes

- The emulator is a valid environment for T025: it reproduces the hardware's
  per-phase costs to within 1%, and it is 6.6× faster, so an A/B run that
  took ten minutes on hardware takes about ninety seconds.
- It is **not** valid for absolute frame-rate claims. T023 already said it:
  "different clock, not a different amount of work."
- The `dbg` phase must be excluded from any emulator-based profile: it is
  8.8% under the emulator and 2.5% on hardware, and it is not guest work.
