---
ticket: T038
phase: plan
status: done
created: 2026-10-21
tier: standard
requires:
  - aes/kanban.md
  - aes/tickets/T038-sdmc-rom-path-resolution.md
produces:
  - aes/tickets/T038-plan.md
blocked_by: ''
---

# T038 — Plan (as executed)

## Reconnaissance Summary

T025 step 1 (`13e16fb`, per-frame instruction tracing) had never produced a
single observation: every build carrying it stopped before the main loop on
the Deck. The plan below was written from the evidence available at the time
and then **revised twice** as measurements contradicted it. Both revisions are
recorded here rather than erased, because the wrong turns are the useful part.

### Evidence available when the plan was written

Five Deck runs under one launcher, observable = the byte size and content of
`/run/media/deck/mmcblk0p1/Emulation/storage/azahar/sdmc/simcity-trace.log`:

| Build | `ROM_PATH` (from `strings`) | Trace bytes | Last line |
|---|---|---|---|
| `simcity.3dsx` (18:17) | `sdmc:/SimCity(USA).sfc` | 665 | `[T6] advance #4: done route=1 rendered=1` |
| `simcity_bridge.3dsx` (22:35) | `sdmc:/simcity.sfc` | 95 | `[T0] fopen: sdmc:/simcity.sfc` |
| `simcity_nobridge.3dsx` (22:02) | `sdmc:/simcity.sfc` | 95 | `[T0] fopen: sdmc:/simcity.sfc` |
| bridge rebuilt with `SimCity(USA).sfc` | `sdmc:/SimCity(USA).sfc` | 173 | `[T0] malloc: OK (524288 bytes)` |
| same, after `cp` ROM → `sdmc:/simcity.sfc` | `sdmc:/simcity.sfc` | 190 | `[T0] ftell before fread: 0` |

`strings` also showed the 18:17 build contains **no `[T0]` markers at all** —
it predates `1ea187a`.

### The two directories

```
/home/deck/.local/share/azahar-emu/sdmc/          <- symlinks only
    SimCity(USA).sfc -> /run/media/.../SimCity(USA).sfc
    simcity.sfc     -> /run/media/.../SimCity(USA).sfc

/run/media/deck/mmcblk0p1/Emulation/storage/azahar/sdmc/   <- the app's trace lands HERE
    SimCity(USA).sfc   524288 bytes
```

The app's own trace file appearing in the media root is the proof of which
directory the running emulator mounts. The symlinks in the other directory
were never in play.

### The two faults, as they were first understood

- **Fault A** — `fopen("sdmc:/simcity.sfc")` returns `NULL` (the file is not
  in the mounted root). `ds3_init()` returns 0, `main()` prints `INIT FAILED`
  to the **console** and blocks on `KEY_A`. The app is alive and waiting, not
  hung — but the console of a detached launch goes nowhere, so the only
  observable was a trace file ending at `boot:`.
- **Fault B** — with the ROM present, a 512 KB `fread` appeared to hang.

## Hostile Analysis (as written, and what survived)

**ASSUMPTIONS**

- `[KNOWN]` The 18:17 build is the control and it works. — measured, 3× today.
- `[KNOWN]` The mounted SD root is the media directory. — the app's trace
  file is created there.
- `[INFERRED]` Fault B is caused by the `[T0]` trace writes interleaving with
  the ROM read on the same emulated device. — **THIS WAS WRONG.** Chain: the
  working build has no `[T0]` calls; the failing build has them and stalls at
  exactly the read that follows the last one. The chain was sound; the
  premise (that the control's success was attributable to its *absence* of
  traces) was never tested. The control flapped 70 → 665 → 70 bytes with no
  code change at all.
- `[ASSUMED]` A 512 KB read from the emulated SD card is feasible at all. —
  **true, and it was the thing that made the whole search look like a
  port bug.**
- `[UNKNOWN]` Whether the `fread` stall is in Azahar's FS service, in
  libctru's `fread`, or in the interaction. — still unknown, and it turned
  out not to matter.

**FAILURE MODE 1 — the fix is credited to the wrong cause.** *This is the one
that happened.* The `fopen`-failure fix and the trace-visibility fix were
real and worth keeping. But the 512 KB read was blamed on the port, and
several rebuilds were spent on chunking. Mitigation attempted: change exactly
one variable per build. That discipline held — it is why the eventual
root cause was findable — but the *control* was not held constant, so every
chunking experiment was measuring the wrong thing.

**FAILURE MODE 2 — the trace log is treated as ground truth for "hung".**
*Also happened.* `deck_drift.py` classified the failing runs as `after-boot`,
the same signature as a genuine hang, so the distinction was invisible.

**FAILURE MODE 3 — an arm is compared against a drifted control.** *Also
happened.* The control flapped. A flapping control is the documented
T035/T036 condition, and it should have been recognised as such the moment
the same binary produced 70 bytes and 665 bytes minutes apart.

**ALTERNATIVE FRAMING** — the question may not be "why does the build fail"
but "why did three people each read a 70-byte trace file as a hang". Under
that framing the valuable output is a trace log that distinguishes
*failed-and-waiting* from *deadlocked*, and a documented rule about which
SD directory the emulator actually mounts. The plan did both, because they
cost almost nothing, and they are what made the fault legible.

**COST OF BEING WRONG: medium.** No source changes; the cost of a wrong
answer is a wasted cycle and a false claim in the record.

**SCOPE BOUNDARY** — in: ROM path resolution; trace-visible init failure;
reaching the main loop. Out: the performance analysis (T025); `svc=0x32`;
`ConfigureNew3DSCPU`; audio; colour; the physical 3DS; Azahar version
changes; `sudo`.

**INVITATION FOR CONTRADICTION** — the strongest objection, and it was right:
the fault might be in the environment rather than the port. The plan
anticipated it ("a flapping control is uninterpretable") but did not act on
it. Acting on it would have saved the chunking experiments.

## Technical Approach (as executed)

### Change 1 — make the init failure observable

Every early-return branch of `ds3_init()` emits a `ds3_trace()` line naming
the step and the reason, and `main()` traces `INIT FAILED` too. The trace
file is the only observable that survives a detached launch, so the failure
has to land there.

### Change 2 — resolve the ROM from a candidate list

`ds3_init()` tries the requested path first, then a list of real names in
the mounted root, tracing each attempt:

```c
static const char *const k_rom_candidates[] = {
    "sdmc:/simcity.sfc",
    "sdmc:/ROM.SFC",
    "sdmc:/SimCity(USA).sfc",
};
```

`ROM.SFC` is there because it is a short 8.3 name; a name with parentheses
has to survive the 3DS FAT driver's long-name handling.

### Change 3 — per-step `[T0]` markers

Each step of the load is traced, so a stall localises to a call. These were
written through immediately with an `fflush` after each one: the whole point
of the trace file is to name the stuck call, and a marker still sitting in a
buffer when the app dies says nothing.

### Steps 4-5 — measure

1. Build the T025 source with changes 1-3; md5 recorded on the dev host.
2. `scp` to the Deck; re-read the md5 **there**; abort on mismatch.
3. Launch via `launch.sh` with a bounded window; read the trace log.
4. Success criterion: trace contains `[T4] loop: entered` and
   `[T6] advance #N: ... rendered=1`.
5. Read `instr_frame` and record its behaviour.

## What the measurement actually showed

The plan's step 4 failed for most of the session, and the diagnosis moved
three times. Recorded in order, because the sequence is the lesson:

1. **"The ROM path is wrong."** True — `sdmc:/simcity.sfc` was not in the
   mounted root. Fixed by the candidate list plus a real file in the root.
2. **"The 512 KB read hangs."** The per-chunk markers showed the file
   position freezing at exactly 49152 bytes, and a size sweep gave a clean
   32 KB → 40 KB threshold. This looked like a FAT cluster limit in the
   emulated card.
3. **"The threshold is not a byte count."** The same 512 KB file read fine
   in some runs and hung in others; the 18:17 control flapped 70 → 665 → 70
   with no code change. A deterministic limit does not flap.
4. **"It is the Azahar configuration."** Holding the ROM file, the `.3dsx`
   and the emulator constant, and varying only
   `~/.config/azahar-emu/qt-config.ini`:

   | `sdmc_directory` | `use_custom_storage` | Result |
   |---|---|---|
   | media dir | `true` | **stalls** |
   | default dir | `false` | **works** |

   The media-dir configuration is what EmuDeck's reference config specifies
   and what had been copied into the live config. Azahar rewrites
   `use_custom_storage` to `false` at startup, so the live config's value is
   ignored — but the *paths* the two configs leave behind differ, and only
   the regenerated one works.

   Confirmed by two controls: the host reads the ROM at 2.6 GB/s with a
   matching md5, and the same ROM and `.3dsx` work under the regenerated
   config.

**The port was never at fault.** The Oct-18 gold control (`d55fe93a…`, which
contains none of the T025 code) failed identically throughout — which was the
single most important measurement, and it was available from the first hour.

### The result

With the working configuration:

```
[T0] fread: 524288/524288 bytes
[T0] ROM loaded OK
[T1] create: rc=1 inst=0x80a2b68
[T4] loop: entered, auto-advance on (START toggles pause)
[T6] advance #4: done route=1 rendered=1 frames 4..5
[T5] instr/frame #10: 13272
[T5] instr/frame #20: 13601
[T5] instr/frame #30: 13426
[T5] instr/frame #40: 10972
[T5] instr/frame #50: 11145
```

Byte-for-byte identical across four runs and across two different binaries
(the 18:17 control and the T025 build). The 19.8% spread is **between frame
indices, not between runs**: the same frame index yields the same value
every time, so the variation is the emulated guest's own workload, not
measurement noise. Mean 12483 against SD-META-003's predicted ≈12 000.

## Affected Files

| File | Operation | Description |
|------|-----------|-------------|
| `frontend/3ds/3ds_main.c` | modify | Trace-visible init failure; candidate ROM paths; `[T0]` markers; `instr_frame` in `state` |
| `aes/tickets/T038-sdmc-rom-path-resolution.md` | create | The ticket, with the final root cause |
| `aes/tickets/T038-plan.md` | create | This plan |
| `aes/shadow/SD-META-004.md` | create | The Azahar-configuration root cause |
| `aes/kanban.md` | modify | T038 done; T025 in-progress |
| `aes/sprints/sprint-01.md` | modify | Ticket table |
| Deck: `~/.config/azahar-emu/qt-config.ini` | modify | Must be the auto-regenerated one; the EmuDeck reference reproduces the stall |

## Testing Strategy

- **Negative control:** the failing configuration must still fail. Verified —
  the EmuDeck reference config reproduces the stall, and the gold Oct-18
  binary fails under it.
- **Control bracketing:** the 18:17 control re-measured in the same session
  as the arm. Verified — 10 advances, same as the arm.
- **Determinism:** the `instr/frame` series must be byte-identical across
  runs. Verified across four runs and two binaries.
- **Integrity:** md5 on both the dev host and the Deck before every run.
  Verified.
- **No ROM in the repo:** `git status` shows no `.sfc`. Verified.

## Verification Criteria

- [x] `ds3_init()` failure is observable in the trace log
- [x] The ROM resolves from a real file in the mounted SD root
- [x] The 512 KB ROM read completes
- [x] The T025 build reaches `[T4] loop: entered` and `[T6] advance #N`
- [x] `instr_frame` captured, with its spread and reproducibility recorded
- [x] Control re-measured in the same session
- [x] md5 verified on both sides
- [x] `make doctor` passes
- [x] Working tree changes confined to `frontend/3ds/3ds_main.c` and `aes/`
- [x] No ROM file in the repository

## Estimation

- Complexity: **medium** as planned, but ~4× the wall clock, because most of
  it went into diagnosis rather than implementation.
- Risk: **medium** — the decisive assumption (that the control was stable)
  was false for most of the session, and that is the finding.
- Blocking dependencies: Deck reachable; a working Azahar configuration.

## Follow-ups

- **T025** unblocked — the interpreter hypothesis in SD-META-003 can now be
  tested against live instruction counts.
- **T024** unblocked the same way.
- **New ticket:** pin the Azahar configuration so it cannot silently regress.
- **New ticket:** the `svc=0x32` fault takes the debug port with it, so the
  `state` verb is unreachable while the app runs; the trace log is currently
  the only measurement channel.
