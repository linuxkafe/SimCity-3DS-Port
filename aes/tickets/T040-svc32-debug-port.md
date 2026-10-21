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

- [x] The `svc=0x32` fault is characterised: which SVC it is, which
      component issues it, and whether it is fatal to the guest or only to
      the network stack
- [x] It is established whether the fault is responsible for the app's
      inability to hold a debug connection, or whether that is a separate
      problem
- [x] If the fault is benign to the guest, the network stack is fixed or
      worked around so the debug port survives it
- [ ] If it is not benign, the port's live-query capability is either fixed
      or its loss is documented as a permanent limitation of the Deck
      measurement path
- [x] The finding is recorded in `docs/` with the evidence

## RESOLVED — the fault is the missing `ndsp:u` service, and it was never what killed the bridge

### What the fault actually is

`svc=0x32` is `SVC::SendSyncRequest` (`src/core/hle/kernel/svc.cpp:2291`).
`raw=0xD8E007F7` decodes to `ResultInvalidHandle`
(`src/core/hle/result.h:40` — description 1015, level 27 Permanent, summary 7
IPC, module 1 kernel).

So the guest is issuing a **synchronous IPC with an invalid service handle**.

The service that handle belongs to is the port's audio output path.
`frontend/3ds/3ds_audio.c` uses the modern **NDSP** API:

```c
void ds3_audio_init(void) {
    ndspInit();
    ...
}
```

and `ds3_audio_pump()` calls `ndspChnWaveBufAdd(0, wave)` **once per frame**.

Azahar does not implement the `ndsp:u` service. The full list of HLE services
it provides is:

```
cam:q cfg:nor csnd:SND dlp:* dsp::DSP err:f fs:USER gsp::* http:C ir:*
ldr:ro mcu::HWC mcu::RTC mvd:std ndm:u nim:* nwm:* pm:* pxi:dev qtm:*
soc:U srv: ssl:C y2r:u
```

`dsp::DSP` and `csnd:SND` are there; `ndsp:u` is not. So `ndspInit()` fails to
open its service, the handle stays invalid, and every per-frame
`ndspChnWaveBufAdd()` fails with `ResultInvalidHandle`.

The observed cadence matches: the port runs at ~2.2 fps under the emulator and
the fault was counted 70 times in 25 s.

### It is benign to the guest and unrelated to the debug port

The port ignores the result of `ndspChnWaveBufAdd`, so the guest keeps
running. With the bridge now working (see below), the fault is demonstrably
**not** what took the debug port down:

```
info -> OK 3dsdbgkit/2 build=ftp16 breaks=0 ftp_listen=5 ftp_errno=0 cclog=0 anlz=1
```

while the same run logs 70 `svc=0x32` errors.

### The bridge was a separate defect

What actually stopped the app from holding a debug connection was **T043**:
the bridge's listen socket was blocking, so the guest's main thread parked in
`accept()` waiting for a host. The symptom was the same — "the debug port is
unreachable while the app is running" — but the cause was in the port, not in
the emulator. Fixed in 129cce7; port 5002 and port 5000 are now open while
the app runs, and `info` answers.

### What this costs, and what it does not

The emulator produces **no audio**: the guest's NDSP path is a no-op there.
That matters for T024 and T025, which measure audio. Audio *timing and CPU
cost* are still measurable — the guest still does all the work of draining
the core's PCM and filling the ring buffer, and the ring buffer's behaviour
is exactly what T025 step 2 was measuring. What is not measurable under the
emulator is the audio *output* (the CSND/NDSP DAC path).

Tracked as **T045**.

## Notes

`raw=0xD8E007F7` looks like a pointer, not an SVC number, which suggests
the guest is issuing a service call with a garbage argument. `summary=7` and
`module=1` narrow it. The 3DS's `fs:USER` and `cfg:u` services are the
candidates worth checking first, since this ticket's symptom is a *file*
access.

T035 and T037 both touched this fault and deferred it. It is worth one
focused attempt now that the app actually runs, because the debug port is
the difference between a measurement channel that works and one that does not.

