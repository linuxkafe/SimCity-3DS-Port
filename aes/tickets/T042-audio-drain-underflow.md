---
ticket: T042
title: Audio drain loop underflows its unsigned counter and hangs the whole port
sprint: sprint-02
priority: critical
status: in-progress
depends_on: T041
created: 2026-10-21
---

# T042 — The audio drain loop hangs the port

## What was found, and how

Found while validating T041 (the headless Azahar build). The headless
emulator loads the ROM, the guest boots, and then the guest stops making
progress on the **second** `ds3_advance_frame()` call. The trace file ends at:

```
[T6] advance #0: done route=1 rendered=1 frames 0..1
[T6] advance #1: begin
```

and never grows again. Not a slow frame — the emulator ran 200 000 emulation
slices in 182 s with the guest in exactly the same place.

### The evidence chain

1. **The guest is looping, not blocked.** Sampling the guest PC through the
   GDB stub over 40 s alternated between two addresses and never settled:
   `0x1019c8` and `0x108248`. A blocked thread would sit on one address.

2. **The addresses resolve**, using the DWARF in `build/simcity.elf`:

   ```
   $ addr2line -f -e build/simcity.elf 0x1019c8 0x108248
   ds3_advance_frame            frontend/3ds/3ds_main.c:501
   simcity_recomp_audio_read    port-core/src/simcity_static_recomp.c:1250
   ```

3. **The disassembly of the loop** (from the guest, via GDB):

   ```
   0x1019c8: cmp   r8, #1024
   0x1019e4: bl    0x108248        ; simcity_recomp_audio_read
   0x1019e8: cmp   r0, #0
   0x1019ec: sub   r8, r8, r0      ; remaining -= n
   0x1019f0: beq   0x1019c8        ; if n == 0, loop forever
   ```

   `n == 0` and `r8` never decreases → infinite loop.

## Root cause

`frontend/3ds/3ds_main.c:490-512`, the audio drain loop:

```c
size_t avail = simcity_recomp_audio_available(g_recomp);
size_t drain_cap = 512u;
size_t remaining = avail < drain_cap ? avail : drain_cap;
while (remaining > 0) {
    size_t take = remaining < 1024u ? remaining : 1024u;
    size_t n = simcity_recomp_audio_read(g_recomp, chunk, take * 2);
    remaining -= n;
    ...
}
```

`simcity_recomp_audio_read(instance, dst, frame_capacity)` returns a count of
**stereo frames**; each frame is two interleaved samples
(`port-core/include/simcity_static_recomp.h:267-273`, and the implementation
at `port-core/src/simcity_static_recomp.c:1246-1264` writes
`dst[count * CHANNELS]` and `dst[count * CHANNELS + 1]`).

Three defects compound:

| # | Defect | Consequence |
|---|---|---|
| 1 | `take * 2` is passed as `frame_capacity`, but the unit is frames | the core may return up to `2 * remaining` frames — more than `remaining` |
| 2 | `remaining -= n` with `remaining` a `size_t` and no clamp | when `n > remaining`, `remaining` wraps to ~1.8e19 and the loop never ends |
| 3 | the sample loop treats `n` (frames) as a sample count | writes `n` samples instead of `2n`, and pairs the wrong L/R |

**The trigger:** the core produces ~534 stereo frames per SNES frame
(32040 Hz / 60 Hz). `drain_cap` is 512, so `remaining` starts at 512 while
`avail` is 534. With `take * 2` = 1024 as the capacity, the core returns
`min(1024, 534)` = **534** frames. `remaining -= 534` → `512 - 534` → **wraps**.

The next iteration asks for 1024 frames, the core has 0 left, returns 0, and
`remaining` never changes. Infinite loop, on the very first frame that
produces more than 512 audio frames.

**This is not emulator-specific.** The same arithmetic runs on real hardware.
It was introduced by commit `7090ccb` ("T025 step 2: throttle audio drain to
512 frames/frame"), which is the most recent commit and has not been through
a playtest. Before that commit the loop drained `avail` frames, so
`take * 2 >= avail` always held and `n <= remaining` — no underflow.

**Defect 3 is older than `7090ccb` and still live.** The DS frontend does it
correctly (`frontend/ds/ds_audio.c:56-58`):

```c
for (size_t i = 0; i < n; i++) {
    ds_audio_push(chunk[i * 2], chunk[i * 2 + 1]);
}
```

The 3DS frontend does not:

```c
for (size_t i = 0; i < n; i += 2) {
    g_audio_ring[pos] = chunk[i];
    ...
}
```

So the 3DS port has been playing the core's audio at **half rate**, with the
left and right channels paired incorrectly, since the audio path was written.

## Acceptance

- [x] The drain loop cannot underflow: `remaining` is clamped, and the loop
      terminates when the core returns 0 frames
- [x] `frame_capacity` is passed in frames, matching the API contract and the
      DS frontend
- [x] The sample loop writes `2n` samples and pairs L/R correctly, matching
      the DS frontend
- [x] The guest runs past frame 50 under the headless emulator, and the trace
      log shows `[T5] pump: ring_avail=N` with a non-zero, plausible N
- [ ] The `[T5] audio avail` / `audio drained` trace values are consistent
      with the ring buffer's contents
- [ ] A regression test exists that exercises the underflow condition
      (`avail > drain_cap`) without needing the emulator

## RESOLVED — fixed in 129cce7, verified under the headless emulator

```
[   T5] audio avail: 7461
[   T5] audio drained: 0 of 7461 remaining
[   T5] pump: ring_avail=324
```

The guest now runs past frame 330 and the pump lines carry non-zero,
plausible values.

**One acceptance criterion is deliberately left open.** The `audio avail`
backlog is *growing* — 7461 stereo frames pending and rising. That is the
throttle doing exactly what T025 step 2 asked for (drain 512/frame against a
core producing ~534/frame, so the ring accumulates) but the backlog is
unbounded, and 7461 frames is 2.3 s of audio. The "consistent with the ring
buffer" criterion and the backlog question belong to **T025**, which owns the
audio work; they are not silently marked done here.

The last criterion — a regression test that exercises `avail > drain_cap`
without the emulator — is also open. The drain loop lives in
`frontend/3ds/3ds_main.c` behind the 3DS frontend's globals, so it is not
currently reachable from a host test. It should be extracted into a small
pure function (drain N frames from a source into a sink) so the underflow
condition can be tested directly. That is a real piece of work, not a
checkbox, and it is left visible.

## Notes

- The fix belongs in the port, not the emulator. The emulator is behaving
  correctly: it is running the guest's instructions faithfully.
- `n == 0` must also terminate the loop. With the capacity fix, `n == 0`
  means the core has nothing left even though `avail` said otherwise; that
  should end the drain, not spin.
