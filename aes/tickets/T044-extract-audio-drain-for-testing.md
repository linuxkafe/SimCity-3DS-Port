---
ticket: T044
title: Extract the audio drain loop into a host-testable function
sprint: sprint-02
priority: medium
status: pending
depends_on: T042
created: 2026-10-21
---

# T044 — Make the audio drain loop testable without the emulator

## Why

T042's bug was a `size_t` underflow in the audio drain loop, in code that
only runs on a 3DS. It took a headless emulator build (T041) and a GDB
backtrace to find, because the loop is not reachable from a host test.

The same class of defect will be found again. The fix should not require
building an emulator.

## What to do

The drain loop lives in `frontend/3ds/3ds_main.c` inside `ds3_advance_frame()`
and is entangled with the 3DS frontend's globals (`g_recomp`,
`g_audio_ring`, `g_audio_write_pos`).

Extract it into a pure function with the shape:

```c
/* Drain up to `drain_cap` stereo frames from the core into the ring buffer.
 * Returns the number of stereo frames written. */
size_t ds3_audio_drain(SimCityRecomp *recomp, int16_t *ring, size_t ring_size,
                       size_t *write_pos, size_t drain_cap);
```

and have the 3DS frontend call it. Then the underflow condition
(`avail > drain_cap`) is a two-line host test:

```c
/* avail = 534, drain_cap = 512: the old code underflowed here. */
assert(drain(fake_recomp_with(534), ring, sizeof(ring), &wpos, 512) == 512);
assert(wpos == 1024);        /* 512 frames -> 1024 samples */
```

The fake only needs to implement `simcity_recomp_audio_available()` and
`simcity_recomp_audio_read()`.

## Acceptance

- [ ] The drain loop is a function with no 3DS globals in its signature
- [ ] A host test covers `avail > drain_cap` (the T042 condition)
- [ ] A host test covers `avail == 0` (the loop must not run)
- [ ] A host test covers the core returning fewer frames than requested
- [ ] The 3DS frontend calls the extracted function and the trace values are
      unchanged

## Notes

The port-core already has a host test harness:
`build/port-core/tests/`. `test_audio_snapshot_continuation.c` and
`test_wram_write.c` are the model.
