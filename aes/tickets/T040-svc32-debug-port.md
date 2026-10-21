---
ticket: T040
title: The svc=0x32 fault takes the debug port down, so the state verb is unusable while the app runs
sprint: sprint-01
priority: medium
status: pending
created: 2026-10-21
tier: standard
depends_on: []
---

# T040 — The debug port dies with the `svc=0x32` fault

## Context

T038 reached the main loop on the Deck for the first time, and immediately
found that the **debug bridge is unreachable while the app is running**:

```
Kernel.SVC <Error> ... svc=0x32 raw=0xD8E007F7 level=27 summary=7 module=1 description=1015
```

repeating about once a second, and the connection to port 5002 is refused.

This is the fault already recorded in T035 as `svc=0x32` (85 occurrences on
the Oct-18 build). It was treated as a background observation because the
builds never reached the main loop — so nobody had tried to use the debug
port while it was firing.

Consequence for measurement: the `state` verb, `fbrd`, `prof` and every
other live query are unavailable. The **trace log is currently the only
measurement channel** on the Deck, which is why T038's per-frame data had to
be read from `[T5] instr/frame` lines rather than from `state`.

## Acceptance Criteria

- [ ] The `svc=0x32` fault is characterised: which SVC it is, which
      component issues it, and whether it is fatal to the guest or only to
      the network stack
- [ ] It is established whether the fault is responsible for the app's
      inability to hold a debug connection, or whether that is a separate
      problem
- [ ] If the fault is benign to the guest, the network stack is fixed or
      worked around so the debug port survives it
- [ ] If it is not benign, the port's live-query capability is either fixed
      or its loss is documented as a permanent limitation of the Deck
      measurement path
- [ ] The finding is recorded in `docs/` with the evidence

## Notes

`raw=0xD8E007F7` looks like a pointer, not an SVC number, which suggests
the guest is issuing a service call with a garbage argument. `summary=7` and
`module=1` narrow it. The 3DS's `fs:USER` and `cfg:u` services are the
candidates worth checking first, since this ticket's symptom is a *file*
access.

T035 and T037 both touched this fault and deferred it. It is worth one
focused attempt now that the app actually runs, because the debug port is
the difference between a measurement channel that works and one that does not.
