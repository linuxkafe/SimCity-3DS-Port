---
id: SD-META-004
title: "A 70-byte trace file that reads like a hang is usually a failure with a discarded console"
synthesis: "When a detached emulator launch stalls, the trace file is the only surviving observable, and its last line may name a STEP THAT SUCCEEDED rather than the one that failed. An init failure reported only via printf to a console that the launcher discards looks exactly like a hang. Make every failure path write to the trace file. Separately: a size threshold that flips between runs is not a threshold -- a flapping control means the endpoint moved, and any arm comparison against it is uninterpretable."
pointer:
  path: "aes/tickets/T038-sdmc-rom-path-resolution.md"
  content_hash: ""
epistemic_state: SUPORTADA
score:
  activation: 1.0
  pinned: false
  centrality: 0.9
provenance:
  generated_by: "T038 (aes-learn)"
  generated_at: "2026-10-21"
  source_ticket: "T038"
  source_conversation: ""
created: "2026-10-21"
last_accessed: "2026-10-21"
---

# SD-META-004: A failed init and a hang look the same when the console is discarded

## The trap

A detached Azahar launch keeps exactly one observable: the file the app
writes to the emulated SD card. On 2026-10-21 the T025 build stopped with
that file at 70 bytes:

```
=== SimCity 3DS trace start ===
boot: rom_path=sdmc:/SimCity(USA).sfc
```

The last line names a step that **succeeded**. The app had in fact failed to
open the ROM and was then blocked in

```c
while (1) { hidScanInput(); if (hidKeysDown() & KEY_A) break; }
```

waiting for a keypress. Because the failure was reported only through
`printf` to a console that a detached launch discards, the trace file could
not distinguish *failed and waiting* from *deadlocked*. Three people read it
as a hang over several hours.

Two further compounds made it worse, and both are worth checking before
diagnosing anything:

- **The ROM was not where the app looked.** The Deck has two `sdmc:`
  directories; the emulated card mounts one, and a symlink of the same name
  in the other is never visible to the emulator.
- **The trace file was being read from the wrong place.** The app writes it
  into the mounted root; the harness was reading a stale copy from the other
  directory. Several "diagnoses" were made against a previous run's file.

## The rule

**Every failure path must write to the trace file, not only to the console.**
The console of a detached launch is discarded; the trace file is the record.
And the trace must name the *failing* step, not the last step that worked.

The fix is small and it is what made the fault legible: trace each failure
branch, and trace the candidate paths that were tried.

## The companion rule: a threshold that flips is not a threshold

The same session produced a clean-looking measurement: reads of 32768 bytes
completed, 40960 hung, and per-chunk markers froze the file position at
exactly 49152 bytes (3 × 16 KB). That is a textbook FAT cluster boundary in
the emulated card, and it drove a whole line of wrong conclusions —
chunked reads, reopen-per-chunk, raw `read()` instead of `fread()`.

The disproof was that **the same binary, the same file and the same emulator
flapped**: the 18:17 control produced 70 bytes, then 665 bytes, then 70
bytes again, with no code change. A deterministic limit does not flap.

That is the T035/T036 unstable-control condition, already documented twice in
this project. The measurement discipline that applies:

> Hold the control fixed and re-measure it in the same session as the arm.
> If the control's own series is not single-valued, no arm comparison is
> interpretable — and, more importantly, the environment has moved, so
> **the fault is not where you were looking**.

The corroborating control here was decisive and cheap: the **Oct-18 gold
binary** (`d55fe93a…`), which contains none of the new code, failed
identically. That single measurement would have ruled the port out at the
start.

## Why this generalises

Every long-running measurement against an emulator, a device, or a remote
service has the same shape:

- The only surviving observable may not distinguish the failure modes.
- A change in the environment can masquerade as a change in the code under
  test.
- The cheapest disproof of "my code is broken" is a **control that predates
  the change**.

This project has now retracted a root-cause claim twice on this pattern
(`ConfigureNew3DSCPU`, "missing SMDH", and now "the FAT cluster limit").
The three failures share a shape: a plausible mechanism, asserted from
correlation, never tested against a control that did not contain the change.

## Actionable rules

1. Trace every failure branch, not just the successes.
2. Never diagnose from a file that might be stale — confirm its mtime and
   that the current run wrote it.
3. Before blaming the code under test, run a control that predates the
   change. It is one build and one run.
4. If a measured threshold changes between runs, stop measuring the
   threshold and go measure the control.
5. Record retracted claims and their disproof, not just the surviving
   explanation.

## Source chunks

- `frontend/3ds/3ds_main.c:594-598` — the `KEY_A` wait loop that made a
  failed init look like a hang
- `frontend/3ds/3ds_main.c:258-300` — candidate ROM resolution, each
  attempt traced
- `frontend/3ds/3ds_main.c` (T038) — every `ds3_init()` failure branch
  now traces
- `aes/tickets/T035-recover-reproducible-good-build.md` — first occurrence
  of the unstable control
- `aes/tickets/T036-drift-detecting-harness.md` — the harness built for it
- `aes/tickets/T037-control-abort-root-cause.md` — second occurrence
- `aes/tickets/T038-sdmc-rom-path-resolution.md` — third occurrence, and
  the one that resolved it
