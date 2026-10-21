---
ticket: T045
title: The emulator produces no audio — NDSP is not implemented, so the guest's audio path is a no-op there
sprint: sprint-02
priority: medium
status: pending
depends_on: T040
created: 2026-10-21
---

# T045 — No audio under the emulator

## The finding

The port plays the core's PCM through the 3DS **NDSP** API
(`frontend/3ds/3ds_audio.c`): `ndspInit()`, `ndspChnSetRate()`,
`ndspChnWaveBufAdd()`.

Azahar does not implement the `ndsp:u` service. It implements `dsp::DSP` (the
DSP itself) and `csnd:SND` (the legacy sound service), but the user-mode
wrapper that libctru's NDSP API talks to is absent. So under the emulator:

- `ndspInit()` fails to open its service
- every per-frame `ndspChnWaveBufAdd()` fails with `ResultInvalidHandle`
  (this is the `svc=0x32` fault T040 was chasing — 70 occurrences in 25 s)
- the guest's audio output is silent

## What is still measurable, and what is not

**Still measurable** — the guest does all the work:

- the core's PCM production rate (~534 stereo frames per SNES frame)
- the drain loop's behaviour and the ring buffer's occupancy
  (`[T5] pump: ring_avail=N`)
- the CPU cost of the audio path

**Not measurable** — the DAC:

- whether the samples are correct
- whether the L/R channels are paired correctly
- anything about the hardware playback path

**This matters because of T042.** The drain loop's unit bug meant the 3DS port
had been writing `n` samples for `n` stereo frames — half rate, with the
channels mispaired. That defect was invisible under the emulator, because the
emulator plays nothing, and invisible on hardware only because nobody was
listening to the output. A measurement channel that is silent cannot catch a
bug in what it is measuring.

## Options

1. **Port the NDSP path to CSND under the emulator.** CSND is implemented
   (`csnd:SND`). The port would need a runtime check: try NDSP, fall back to
   CSND. This makes the emulator's audio real and would have caught T042's
   third defect immediately.
2. **Implement `ndsp:u` in Azahar.** Larger, and it belongs upstream.
3. **Accept the limitation and document it.** The cheapest, and honest — but
   it leaves the port's audio output unverified.

## Acceptance

- [ ] A decision is recorded: which option, and why
- [ ] If option 1: the port falls back to CSND when `ndspInit()` fails, and
      the emulator produces audio
- [ ] Whatever is decided is recorded in `docs/` so that no audio measurement
      under the emulator is mistaken for an audio measurement of the output
      path

## Notes

Do not read the emulator's silence as "the port's audio is fine". It is
evidence of nothing either way.
