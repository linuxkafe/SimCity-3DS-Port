# VISION — SimCity for Nintendo 3DS

> Last revised 2026-10-17. The previous version of this document described a
> Nintendo **DS** target. The active target is the **3DS**; the DS frontend
> still exists in `frontend/ds/` but is far behind and is not where the work is.

## Problem

SimCity SNES Static Recomp 1.4.0 is a static recompilation of the original
SimCity (USA) SNES ROM. It runs natively on x86_64. It is not portable: it is
generated ARM/x86 code built around a desktop SDL2 frontend and desktop
assumptions about speed and memory.

The goal is a port that runs the original game on 3DS homebrew hardware —
preserving the static-recomp certification guarantees, in particular that the
generated dispatch shards stay byte-identical — while making the host-side glue
fast enough to be playable on a 268 MHz ARM11.

## Solution

- **Reuse the generated dispatch unchanged.** Shards v11/v34/v35/v36 are
  ROM-certified and stay byte-identical to upstream. The upstream core is
  modified only through `patches/`, never edited in place, so any build is
  reproducible from a clean checkout.
- **Write a libctru frontend.** `frontend/3ds/` provides the frame loop, video,
  input and audio, replacing the SDL2 frontend.
- **Make the host-side glue measurable.** The emulated CPU runs at roughly
  12 000 guest instructions per frame, which cannot account for the per-frame
  tick counts observed. Most of the cost is in the PPU composite, the audio
  DSP, and diagnostics. Finding that is done by measurement, never by guessing
  at which phase looks expensive.
- **Fail closed.** Unknown execution paths abort with a diagnostic rather than
  producing wrong output. No interpreter, no JIT, no dynamic recompilation.
- **Build in Docker** (`devkitpro/devkitarm`) so the host needs only Docker.
- **Test headless before hardware.** Runs under Azahar with guest TCP working,
  so the debug bridge is reachable from the host with no physical interaction.

## What makes this hard

Not the porting. The interesting constraint is that **the profiling is
hostile**. The bottleneck is host-side work in generated code, not in the
emulated CPU, so the usual instincts — look at the inner loop, count
instructions — point at the wrong place. Two of the first three performance
results in this project were wrong because of it, and one had to be retracted.

The second difficulty is that **`core` is not a stable clock**: it varies by up
to 42% with scene content. A measurement that does not control for that will
confidently report nonsense, and did.

## Value

A native, deterministic, fail-closed SimCity on 3DS hardware, running the
original code rather than an approximation of it — plus a debug bridge that is
reusable in any libctru homebrew.

## Success, concretely

- The game boots and renders correctly on hardware and under emulation.
- A playable frame rate on 3DS, with every optimisation traceable to a
  measurement that controlled for drift.
- Audio audible at the headphone jack.
- Frame output provably identical to the original for the same ROM and input
  sequence, verified by hash rather than by eye.
- An automated test path that needs no physical interaction.
