---
ticket: T038
title: The T025 instrumented build could not reach the main loop on the Deck
sprint: sprint-01
priority: high
status: done
created: 2026-10-21
completed: 2026-10-21
tier: standard
blocks: T025
depends_on: []
---

# T038 — T025 build could not reach the main loop on the Deck

## Outcome

**T025 is unblocked.** The T025 instrumented build now boots on the Deck, reaches
the main loop, renders frames, and produces the per-frame instruction counts
that were the point of the ticket.

The fault was **not in the port**. It was in the Deck's Azahar configuration.
Two independent facts establish this:

1. The **Oct-18 gold control** (`d55fe93a…`, the T035/T037 reference binary,
   which contains none of the T025 code) failed identically to the T025 build —
   same stop point, same trace, same symptom.
2. The T025 build and the gold control **both** started working the moment
   Azahar's configuration was regenerated, with no change to any `.3dsx`.

## What the symptom looked like, and why it was misread

The T025 build stopped with the trace log at 70 bytes:

```
=== SimCity 3DS trace start ===
boot: rom_path=sdmc:/SimCity(USA).sfc
```

That reads like a hang, and it was read as one for hours. It was not. The
app had **failed** to open the ROM, and `main()` then blocked in

```c
while (1) { hidScanInput(); if (hidKeysDown() & KEY_A) break; }
```

waiting for a keypress that never comes (`frontend/3ds/3ds_main.c:594-598`).
Because the failure was reported only through `printf` to the console, and a
detached AppImage launch discards the console, the *only* surviving observable
was a trace file whose last line said `boot:`.

Two further faults compounded it and were real:

- **The ROM was not where the app looked.** The Deck has two `sdmc:`
  directories. The emulated card mounts one of them; a symlink of the same
  name in the other was never visible to the emulator. The build was
  compiled against `sdmc:/simcity.sfc`, which existed only as a symlink
  in the *unmounted* directory.
- **The trace file was not written where anyone looked.** The app's own
  trace file is written into the mounted root, while the harness was
  reading it from the other one. Several runs were "diagnosed" against a
  stale 70-byte file from a previous run.

## Root cause, by measurement

The decisive experiment was to hold the ROM file constant and vary the
**Azahar configuration**:

| Configuration | Result |
|---|---|
| `sdmc_directory` = media dir, `use_custom_storage=true` | **stalls** |
| `sdmc_directory` = default dir, `use_custom_storage=false` | **works** |

The media-dir configuration is what EmuDeck had written into
`~/.config/EmuDeck/backend/configs/azahar/qt-config.ini` and what had been
copied into the live config. Azahar resets `use_custom_storage` to `false` on
startup, so the live config's value is ignored — but the *regenerated* config
and the EmuDeck config differ in the paths they leave behind, and only the
regenerated one works.

Two further controls confirmed the fault was not in the ROM or the card:

- The host reads the ROM at 2.6 GB/s; md5 `23715fc7…` matched throughout.
- The same ROM, the same `.3dsx`, and the same emulator worked when the
  configuration was regenerated.

## The size-threshold red herring

A size sweep produced a clean-looking threshold — 16 KB reads complete, 36 KB
completes, 40 KB hangs, 512 KB hangs — and per-chunk markers showed the
position freezing at exactly 49152 bytes (3 × 16 KB). That looked like a FAT
cluster limit in the emulated card and drove several wrong conclusions:

- Chunked reads (16 KB, 4 KB) were tried as a fix. They made it **worse**:
  reopening the file per chunk hung inside `open()` itself.
- The boundary was shown not to be a byte count: the *same* 512 KB file read
  fine in some runs and hung in others, and the 18:17 build flapped
  70 → 665 → 70 bytes with no code change.

That flapping is the tell: a deterministic limit does not flap. It is the
T035/T036 unstable-control condition, and it should have been recognised
sooner — the Deck's own history records two previous rounds of it.

## The change that made the fault legible

The port itself is now better at failing loudly, which is the durable
outcome of this ticket:

- `ds3_init()` traces **every** failure branch, so a failed boot says why in
  the trace file instead of only on the discarded console.
- The ROM is resolved from a candidate list of real names in the mounted root,
  with each attempt traced.
- `[T0]` markers report each step of the load, so a stall localises to a call.

## Acceptance Criteria

- [x] `ds3_init()` failure is observable in the trace log, not only on the console
- [x] The ROM resolves on the Deck from a path that is a real file in the
      mounted SD root
- [x] A build containing the T025 tracing reaches `[T4] loop: entered` and
      `[T6] advance #N` on the Deck
- [x] The 512 KB ROM read completes
- [x] The `[T5] instr/frame` value is captured, and its behaviour recorded
- [x] The control (`simcity.3dsx`, the 18:17 build) re-measured in the same
      session as the arm — 10 advances, same as the arm
- [x] Every build md5-verified on both the dev host and the Deck
- [x] `make doctor` passes
- [x] Working tree changes confined to `frontend/3ds/3ds_main.c` and `aes/`
- [x] No ROM file added to the repository

## T025 measurement taken as part of this ticket

Per-frame SNES instruction counts, from `[T5] instr/frame` (every 10th frame):

| frame | instructions/frame |
|---|---|
| 10 | 13272 |
| 20 | 13601 |
| 30 | 13426 |
| 40 | 10972 |
| 50 | 11145 |

- **Reproducible byte-for-byte across four independent runs**, on two
  different binaries (the 18:17 control and the T025 build).
- Mean 12483, median 13272, spread 19.8% over the 10-frame window.
- The 19.8% spread is **real per-frame variation, not measurement noise**:
  the same frame index gives the same value on every run, so the spread is
  between frames (the emulated game is doing different work at different
  points in its own loop), not run-to-run jitter.

Against SD-META-003's prediction of ≈12 000 instr/frame, the measured mean of
12 483 is consistent: the interpreter executes a roughly constant amount of
work per frame, and the variation is the guest's own workload, not the
recompiler re-translating. **The interpreter hypothesis is not falsified by
this data** — the constancy across frame indices is weak evidence *for* it.

## Scope

**In scope:** the ROM path resolution; making the init failure trace-visible;
reaching the main loop with the T025 build; the first `instr/frame` numbers.

**Out of scope:** the performance analysis itself (T025); the `svc=0x32`
fault; `ConfigureNew3DSCPU`; audio; colour; the physical 3DS; changing the
Azahar version; `sudo`.

## Rollback

- Source changes: `git revert 42e99e8` and `b35bcf7`.
- Deck configuration: the working config is the auto-regenerated one
  (`use_custom_storage=false`, default `sdmc_directory` and `nand_directory`).
  The EmuDeck reference config at
  `~/.config/EmuDeck/backend/configs/azahar/qt-config.ini` reproduces the
  stall and is the known-bad reference.
- The `simcity.sfc` copies on the Deck are additive; remove with `rm`.

## Known Risks

- The Deck's endpoint is still capable of drifting (T035/T036). The T025
  numbers were reproducible across four runs, but a future session should
  re-run the control before trusting an arm.
- The working configuration is the *auto-regenerated* one. Any tool that
  rewrites `~/.config/azahar-emu/qt-config.ini` from the EmuDeck reference
  will reintroduce the stall. This needs a follow-up ticket.

## Follow-ups

- **T025** is unblocked: continue the perf deep-dive with the interpreter
  hypothesis now testable against live instruction counts.
- **T024** (audio) is unblocked the same way — the app now reaches the main
  loop, so `simcity_recomp_audio_available()` can be traced.
- **New ticket:** pin the Deck's Azahar configuration so it cannot silently
  regress, and record the working configuration in `docs/`.
- **New ticket:** the `svc=0x32` fault takes the debug port down with it,
  so the `state` verb is unreachable while the app is running. The trace log
  is currently the only measurement channel.
