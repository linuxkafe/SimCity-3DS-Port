---
ticket: T047
title: 30 fps — what it actually costs, and the two independent problems it decomposes into
sprint: sprint-02
priority: high
status: in-progress
depends_on: T046
created: 2026-10-21
---

# T047 — The 30 fps target

## The target

30 fps on the 3DS device. The port currently runs at **1.59 fps** (T023) and
the guest needs **3.86 s of 3DS-clock time per SNES frame** (T046).

## Measured, on the Deck, with the tooling that already exists

The upstream ships a native x86 profiler,
`/home/seyon/dev/Games/PC/SimCity-SNES-Static-Recomp/build-prof/frontend/linux/headless-profile`,
with a 1 KiB PC histogram and `--window A:B`. It runs on the Deck unchanged —
the ROM is already there, so **nothing has to be copied off the device**.

### Native x86, city view, 600 frames (`gprof`, `-pg -O2`)

```
 29.02%  sc_machine_nonzero_wram    <- gprof artefact, see upstream T039
 28.23%  background_pixel
 16.09%  render_lines_impl
  6.07%  screen_pixel
  3.69%  render_current_frame
  2.64%  sc_v11_scheduler_code_access
  1.06%  sc_v11_write_raw
  0.53%  sc_v11_cpu_step
```

**The CPU emulation is ~2-3 % of the time. The renderer is ~71 %.** This
independently reproduces the upstream's own decision
`docs/decisions/T039-render-is-the-bottleneck.md`, which also establishes:

| Native x86 | Cost |
|---|---|
| Pure emulation | 3.57 ms/frame (~280 fps) |
| Rendering | ~8.6 ms/frame |
| **Total** | **12.2 ms/frame (~82 fps)** |

The PC histogram for the same window puts 94 % of the emulated time in two
1 KiB SNES code regions (0x09000 and 0x08C00) — the game's own code, which
is where it calls into the PPU.

### On the 3DS

| Component | 3DS cost | Native x86 | Ratio |
|---|---:|---:|---:|
| Emulation | ~1.13 s | 3.57 ms | 316× |
| Rendering | ~2.72 s | 8.6 ms | 316× |
| **Total** | **3.86 s** | **12.2 ms** | **316×** |

The split is the same on both machines: **rendering is ~70 % of the frame.**

## What 30 fps actually requires

30 fps = 33.3 ms per SNES frame of 3DS-clock time. From 3.86 s that is a
**116× reduction**, and it decomposes into two independent problems:

| | Required | Available from render work alone |
|---|---:|---:|
| Rendering | 33.3 ms (from 2.72 s) | **82× — achievable** |
| Emulation | 33.3 ms (from 1.13 s) | **34× — not achievable by tuning** |

**The floor.** 33.3 ms at 268 MHz is 8.9 M ARM11 cycles per SNES frame, and
the frame is 12,283 SNES instructions. So:

> **30 fps on the 3DS = 725 ARM11 cycles per SNES instruction.**

That is a *normal* budget for a real 65816→ARM11 static recompiler (typical
is 10-100 cycles per instruction). It is not an exotic target.

**The problem is what this codebase is.** The "static recompilation" is a
per-address generated dispatch in portable C: a switch on the exact PC, with
the state round-tripping through memory between instructions. Natively that
costs ~1 µs per SNES instruction (~3,500 x86 cycles) — 30-100× more than a
real static recompiler. On the ARM11 that becomes thousands of cycles per
SNES instruction.

**So 30 fps on the device is reachable, but only by making the emulation
actually static — translating SNES blocks to ARM11 that operate on the CPU
state in registers, with no per-instruction dispatch. That is a recompiler
project, not a tuning task. There is no cheaper path.**

## The good news: the emulator is a 30 fps target today

The emulator runs the guest's clock **42× faster than wall time**, so:

| | Frame rate |
|---|---:|
| 3DS device, now | 0.26 fps (emulated clock) / 1.59 fps (wall) |
| 3DS device, render free | 0.86 fps |
| Emulator, now | 11.0 fps |
| **Emulator, render free** | **~36 fps** |

**The dirty-tile render alone takes the emulator past 30 fps.** That is the
2.7× the emulator needs, and it is work that is already planned upstream
(`T030-dirty-tile-render.md`, open, with a closure condition) and that
upstream T039 identifies as the highest-value optimisation available.

## What is *not* the problem

- **The port's frontend.** `core` is the whole emulation core; the port's own
  code (`render`, `hud`, `audio`, `present`) is 1.6 % + 0 % + 0 % + 2.1 %.
- **The emulator.** It is 99 % of one core and it reproduces the hardware's
  per-phase costs to within 1 % (T046). It is the measurement environment,
  and it is done.
- **The CPU dispatch.** ~2-3 % natively. Optimising the "static
  recompilation" as T025's title suggests would be wasted effort — for the
  *rendering* half of the problem.

## Recommendation

1. **Take the upstream's open T030 (dirty-tile render) first.** It is the
   cheapest large win, it is already planned, it takes the emulator from
   11 fps to ~36 fps, and it removes ~70 % of the 3DS frame. Validate every
   step with `headless-profile` natively and with the port's frame-hash
   harness (T032) so the output is provably unchanged.
2. **Decide the device target explicitly**, because it is a different project:
   - *"30 fps in the emulator for measurement and playtest"* → T030 plus a
     little. Weeks, not months.
   - *"30 fps on the 3DS device"* → the recompiler has to become a real
     static recompiler. That is a rewrite of the emulation core, in the
     **upstream** repo, and it needs its own plan.
3. **Correct the port's model** (T023, T025) before either: "core is 92.7 %
   of the loop" is true at the phase level and useless for prioritisation.
   The function-level truth is that the CPU is ~2 % and the renderer is
   ~71 %.

## UPDATE — upstream T059: T030's mechanism was the wrong one, and the right one is underway

T030's dirty-tile bitmap tracks **VRAM** writes. But the city map and the
city camera live in **WRAM** (`SC_CITY_MAP_WRAM_OFFSET`,
`SC_CITY_VIEW_X_OFFSET`). A renderer consulting a VRAM dirty bitmap is blind
to camera movement and to map changes, and would emit a stale city — T037
recorded this and it was never resolved.

So T030 was reopened upstream as **T059**, with the other half of its goal
("do not recompute what is already known") and none of the dirty-marking
correctness surface:

- the per-line setup computes `mode`, `mosaic_size` and `mosaic_enable`;
  the per-pixel code re-read 0x2105 and 0x2106 (twice) per pixel per layer
  and overwrote the correct `mosaic_size` with a recomputed copy of itself
- `wrap_coordinate` for `world_x` was a signed 32-bit division per pixel per
  layer, although `world_width` is always a power of two and the existing
  `world_y` wrap already masked on that basis
- the tilemap entry was resolved per pixel although it is a function of
  (tile_x, tile_y) alone within a line
- `city_map_tile_entry()` — two WRAM reads and a ROM read — was called per
  pixel although the map is 8×8 cells

**Result** (upstream `0c31e4e`):

| | before | after | |
|---|---:|---:|---:|
| native x86, us/frame | 8978.9 | 7979.4 | 1.13× |
| 3DS, guest clock per frame | 67.1 s | 57.1 s | 1.18× |

Output provably unchanged: the 343-entry `--hash` stream and all 73 `--shots`
BMPs are byte-identical to the baseline, and ctest is 4/4.

**1.18× is not the 2× T059 asked for, and the ticket stays open.** The
re-profile says why — the render path is no longer the bottleneck. The
largest remaining item is now:

```
 24.16 %  raw_clock (sc_v11_scheduler.c:79)
```

`sc_v11_scheduler_advance()` steps the PPU clock one master clock at a time,
**357,000 times per frame**, for a timing model that only acts at a line
boundary, at the NMI line, and at the auto-joypad deadline.

**So the sequence of work is: render (done, 1.18×) → scheduler (next) →
and only then does the "is the emulation a real static recompiler" question
matter.** The scheduler is emulation, so it moves the T047 floor in the
right direction, but it is not a rewrite.

## UPDATE — the T030 series is done: 1.38× native, 1.33× on the 3DS, and the emulator passes 30 fps

Three upstream tickets, all output-identical against the pre-T059 baseline
(343-entry `--hash` stream and all 73 `--shots` BMPs byte-identical over 900
frames of the T048 city input cadence, ctest 4/4):

| upstream | what | native | 3DS |
|---|---|---:|---:|
| T059 | hoist the per-pixel work the per-line setup already did; cache the per-pixel map lookups per tile | 1.13× | 1.18× |
| T060 | cache the tile *data* bytes per tile row instead of per pixel | +1.03× | — |
| T061 | step the PPU clock to the next event instead of one master clock at a time | +1.13× | — |
| **total** | | **1.38×** | **1.33×** |

Commits: `0c31e4e` (T059), `59c19fc` (T060), `7eea606` (T061).

### CORRECTION — the emulator runs at 14 fps, not 71.9

The "71.9 fps" figure was **my measurement script parsing the `state` verb's
decimal values as hex**. Corrected, the headless build on the 3DS runs the
guest at **~14 fps** (title screen), and the emulator's guest clock runs at
42x real time. The 3DS numbers in the earlier update were wrong for the same
reason and are withdrawn.

### The emulator target is NOT met

| | fps | guest clock per frame |
|---|---:|---:|
| headless build, title screen | ~14 | 3.007 s |

**30 fps in the emulator is still ~2x away**, not achieved.

### The 3DS benefit of T059-T062 is unproven

The native measurement is solid — 1.43x, byte-identical output, same input
cadence. On the 3DS, four builds with very different cores report almost
the same `core` phase (2.503 / 2.503 / 2.442 / 2.487 s) and exactly the
same `present` phase (82.4 ms — five 60 Hz VBlanks — to the millisecond in
every build).

Natively the same work is 5,563 us against 7,984 us; on the 3DS it is 2.5 s.
That is a 350x ratio where the clock ratio alone accounts for 13x, so the
ARM11 runs this code ~27x less efficiently per instruction than x86-64.

**That is a cache-miss-bound loop, not an instruction-bound one — and
everything T059-T062 removed was instructions.** So the optimisations that
won 43 % on x86 do not transfer to the device.

**The lever on the 3DS is the working set, not the instruction count.**

### The device target is not, and the gap is now precisely known

The guest still needs **50.36 s of 3DS-clock time per SNES frame** in the
live city (down from 67.14 s). On the device that is still 50.36 s, so the
device frame rate is unchanged in order of magnitude.

The 30-fps device target needs 33.3 ms per frame — a further **1,500×**.
T047's floor calculation stands: 30 fps on a 268 MHz ARM11 is 725 ARM11
cycles per SNES instruction, which is a normal budget for a *real* 65816→ARM11
static recompiler and ~30-100× better than what this codebase does. That is
a rewrite of the emulation core, not a tuning task, and it is the only thing
left on the path to 30 fps on the device.

### What is left in the frame, after the series

The render path is no longer the bottleneck and the scheduler is no longer
the bottleneck. The next profile would be needed to say what is — the
remaining items were a long tail of 1-4 % lines, plus `snapshot_hash_bytes`
at 1.7 % (the frame cache hashing 64 KiB of VRAM every frame).

## Acceptance

- [x] The target is decomposed into its two independent components
- [x] The floor is established: 725 ARM11 cycles per SNES instruction
- [x] The upstream's existing analysis is confirmed by independent measurement
- [x] The upstream's T030 (dirty-tile render) is identified as the first move
- [ ] The device-vs-emulator target is chosen and recorded
- [ ] T023/T025's model is corrected
- [ ] The chosen target has a plan with measurable closure conditions

## Notes

- All of this is measured on the Deck with tools that already exist. No ROM
  was copied off the device; the native profiler runs there because the Deck
  is x86_64.
- The fixes live in the **upstream** repo
  (`/home/seyon/dev/Games/PC/SimCity-SNES-Static-Recomp`), which the port
  consumes read-only. The port repo cannot fix the core.
