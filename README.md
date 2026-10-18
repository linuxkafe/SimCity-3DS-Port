# SimCity for Nintendo 3DS

A port of **SimCity** (1991, Maxis / Will Wright) from the Super Nintendo to
Nintendo 3DS homebrew, built on a static recompilation of the original game
rather than an emulator or an interpreter.

> **⚠️ Status: runs, but is not playable, and is not yet fast.**
>
> The game boots, renders correctly on hardware and in an emulator, and accepts
> input. It does not run at a playable frame rate yet. No audio. There is no
> test coverage except what is described below.
>
> **No ROM is included and none will ever be.** See [Legal](#legal).

---

## What game this is

**SimCity**, the 1991 city-building game that founded the genre, as released on
the Super Nintendo in 1991. Not SimCity 2000, not the SNES re-release — the
original. The target is specifically the **USA** release, which is the ROM the
upstream static recompilation was built against.

You build the city by zoning residential, commercial and industrial land,
watching it grow, and dealing with the consequences: traffic, pollution,
taxes, fires, and your own zoning mistakes. It is a construction toy with a
systems simulation underneath, and it is still the sharpest example of that
design.

Because this port runs the original code, the game *is* SimCity — same
behaviour, same graphics, same simulation. Nothing here reimplements the game
logic.

## What this project is not

- **Not an emulator.** There is no interpreter, no JIT, no dynamic
  recompilation. The original game code is translated ahead of time into
  native ARM11 machine code that runs directly.
- **Not a ROM distribution.** See [Legal](#legal).
- **Not finished.** See [Status](#status--measured).

## How it works

The upstream project provides a static recompilation of SimCity (USA) as
hand-written C plus a large set of generated dispatch shards. This port:

1. **Reuses that core.** It compiles and runs as-is on ARM11. The generated
   shards (v11/v34/v35/v36) are ROM-certified and are kept byte-identical to
   upstream — nothing in this repository modifies them.
2. **Patches it, never edits it.** `patches/` holds a chain applied on every
   build, so any build is reproducible from a clean checkout. See
   [Patch chain](#patch-chain).
3. **Writes its own 3DS frontend** — `frontend/3ds/` — using libctru: video,
   input, audio, and a debug bridge.
4. **Carries a debug bridge**, `3dsdbgkit`, over TCP. It started as the way to
   inspect a running port and became a reusable component in its own right,
   with a GDB RSP stub alongside a line-oriented command protocol.

### Why static recompilation

The original runs at the speed of a 1991 Super Nintendo. Transliterating it to
hand-written C would be a rewrite and would drift from the original over time.
Static recompilation keeps the fidelity: every instruction, every table, every
timing quirk, translated mechanically. The result is a program that is
genuinely the original game, compiled for a different CPU.

The cost is that the generated code is large and the profiling is unusual — most
of the per-frame time is in host-side glue, not in the emulated CPU. That
shapes most of the work in this repository.

## Status, measured

Performance work so far, all measured in ARM11 ticks per frame. `core` is
`simcity_recomp_advance()`.

| Change | Effect | Evidence |
|---|---|---|
| Core log off by default (removes 2× per-frame SHA-256) | **−14.5%**, active | 12 interleaved pairs, +21.8M ticks |
| `bgra()` integer divides → exact 32-entry table | shipped | output proven identical, 0/32768 divergences |
| Read frame as BGR555 instead of re-widening BGRA | **no measurable gain** | −1.1%, inside noise |

Three things you need before trusting any number in this table:

1. **`core` is not a stable clock.** It varies by up to **42%** with scene
   content. Measuring change A and then change B minutes apart measures that
   drift, not the change. Every real result here uses interleaved pairs with
   the difference taken *within* each pair, and discards contaminated pairs
   rather than averaging them in.
2. **An earlier −23.7% figure was retracted.** Drift alone moves 50M ticks
   against a reported 46M effect. The change stayed — it is provably
   output-identical and removes 183 584 divides — but the number was not sound.
3. **Output equality has never been verified on hardware.** The colour
   conversion is proven exhaustively on the host over all 32768 possible pixel
   values, which cannot catch a stride or rotation error. On-device equivalence
   needs a same-frame capture from both builds and has not been done.

**Still unattributed:** the large majority of per-frame ticks, spread across the
static route loop, `smp_sync_to_master` (the DSP) and the composite pass.
Optimising before finding that is what produced the −1.1% row above.

### Patch chain

| Patch | Purpose |
|---|---|
| `0001-cold-reset-sram-on-heap.patch` | ARM11 portability for the cold reset |
| `0002-core-bgra-without-divides.patch` | replaces per-pixel divides with an exact table |
| `0003-core-expose-bgr555.patch` | exposes the composited frame the core already builds |
| `0004-core-switchable-analyze.patch` | makes `analyze()` measurable |

## Building

```bash
make docker-build      # inside Docker (devkitpro/devkitarm)
```

You need a legally obtained **SimCity (USA)** `.sfc` at `~/SimCity(USA).sfc`.
The build does not embed it; the frontend loads it at runtime.

`make doctor` checks the environment. See `Makefile` for the other targets.

## Running it on hardware

The frontend expects the ROM on the SD card at `sdmc:/SimCity(USA).sfc`, and
listens for the debug bridge on TCP **5002**. Host-side helpers live in
`tools/`:

| Tool | Purpose |
|---|---|
| `deck_ab.py` | paired A/B performance measurement (see below) |
| `netfb.py` | pull live pixels off the device as PNG |
| `play.py` | drive the game and capture after draining frames |
| `netdbg.py` | host-side GDB attach |
| `raw2png.py` | convert raw framebuffer dumps |

## Running it headless

The port runs under **Azahar** (a Citra fork), which gives a repeatable test
target with no physical interaction. Guest TCP works: the debug bridge binds and
is reachable from the host.

This was verified rather than assumed — emulators are commonly expected *not*
to provide guest networking, and the whole harness design depended on the
answer.

**Known defect on this target: the debug bridge can wedge.** When a client
disappears without a clean close, the bridge can hold its single-client slot
indefinitely and stop answering, while the port still appears to `LISTEN` — so
a reachability check passes and the real symptom is silence. A `poll(POLLHUP)`
reap was attempted (`8e8b8b9`) and **did not resolve it**; that claim is
retracted. Short interactive sessions are unaffected. Long automated sessions
are not, and this is the one thing standing between the project and a reliable
CI gate.

One important limit: **tick counts in an emulator are not the same clock as
tick counts on hardware.** They are useful for functional tests and for effects
well above the noise, and misleading for small ones. `deck_ab.py` reports
`UNRESOLVABLE` rather than a number when an effect is below the noise floor,
because that is what it means.

```bash
python3 tools/deck_ab.py --host <target> --switch cclog --pairs 12
```

## Legal

This repository contains **no ROM, no ROM image, and no copy of Nintendo or
Maxis code**. Two copyrighted `.cia` dumps were accidentally committed in the
initial commit; that history was rewritten and the repository recreated without
them. The `.gitignore` rules that were supposed to prevent it could not, because
a `.gitignore` pattern does not apply to an already-tracked path.

You must supply your own legally obtained copy of SimCity (USA). The game is
copyrighted by Maxis/Nintendo; this project is an unofficial port and is not
affiliated with or endorsed by either.

## Layout

```
SimCity-3DS-Port/
├── frontend/3ds/        3DS frontend: main loop, render, input, audio
│   ├── 3ds_main.c       frame loop, profiling, lifecycle
│   ├── 3ds_render.c     RGB565, rotation, channel order
│   ├── 3dsdbg.c/h       the debug bridge (GDB RSP + command protocol)
│   └── ds3ftp.c/h       in-app FTP, for shipping builds to the SD card
├── frontend/ds/         Nintendo DS frontend (separate target, less advanced)
├── patches/             the patch chain, applied on every build
├── src/                 the static-recompiled core (upstream; see note)
├── tools/               host-side helpers
├── investigation/       experiments, including ARMv7/3DSX packing
├── docs/                vision, requirements, roadmap, port study
├── CMakeLists.txt       cross-compile for ARM11 (3DS) and ARM9 (DS)
└── Makefile             docker-build, doctor, and friends
```

`src/` and the generated shards come from the upstream static recompilation and
are modified only through `patches/`, never in place.

## Related

- **Upstream**: SimCity SNES Static Recomp — the static recompilation this port
  is built on.
- **3dsdbgkit**: the debug bridge in `frontend/3ds/3dsdbg.c` is generic and
  reusable in any libctru homebrew. It has no SimCity dependency.
