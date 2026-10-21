---
ticket: T043
title: The debug bridge's listen socket is blocking, so the game freezes whenever no host is attached
sprint: sprint-02
priority: critical
status: in-progress
depends_on: T041
created: 2026-10-21
---

# T043 — The port freezes waiting for the host to connect

## Symptom

Under the headless emulator (T041), the guest boots, loads the ROM, renders
frames, and then **stops dead at guest frame 60**. The trace file stops
growing. The emulator keeps spinning emulation slices (200 000 slices in 182 s)
with the guest in exactly the same place.

`g_frame_count` read out of the guest's memory through the GDB stub:

```
0xd951e4:  0x0000003c      <- g_frame_count = 60
0xdaaa1c:  0x00000001      <- g_dbg_trace_perframe = 1
0xd951d0:  0x0000003c      <- s_tick = 60
```

`g_frame_count` did not move across three samples taken 10 s apart. The guest
is frozen, not slow.

## Root cause

`lr` pointed at `accept`, and the DWARF in `build/simcity.elf` named it:

```
$ addr2line -f -e build/simcity.elf 0x00d23d90
accept
libctru/source/services/soc/soc_accept.c:46
```

`frontend/3ds/3dsdbg.c:1174-1176`:

```c
if (g_client_fd < 0) {
    int c = accept(g_listen_fd, NULL, NULL);
    if (c < 0) { g_st_accept_fails++; g_st_accept_err = errno; }
```

`dbgk_poll()` runs **once per frame** from the main loop, and the port's own
comment at `frontend/3ds/3ds_main.c:826` says what it is supposed to be:

> Host debug channel: service the TCP server (non-blocking).

But the **listen** socket was never put into non-blocking mode. Only the
*accepted client* is:

```c
fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) | O_NONBLOCK);   /* client only */
```

libctru's 3DS `socket()` creates blocking sockets. So `accept()` on the
listener parks the guest's main thread until a host connects. The port's
contract — the game keeps running whether or not anyone is on the other end —
was not implemented.

**This is not emulator-specific.** The same code freezes a real 3DS the moment
the host stops holding a connection to port 5002. Every earlier measurement
that reported the guest running (T031's `frame=196`) had a host attached,
which is why the bug was never visible: with a client connected, `accept()`
returns immediately.

It also explains the long-standing "Deck stall" that T038 chased: the stall
was never the emulator's storage configuration, it was the port's own
`accept()`.

## Fix

1. Put the **listen** socket into non-blocking mode, right after `socket()`
   (`frontend/3ds/3dsdbg.c`).
2. Stop counting `EAGAIN`/`EWOULDBLOCK` from `accept()` as an accept failure.
   "Nobody has connected yet" is the normal state of a listening socket; the
   `pollstat` counters are only useful if they distinguish it from a real
   error.

## Acceptance

- [x] The listen socket is non-blocking
- [x] `EAGAIN`/`EWOULDBLOCK` from `accept()` is not counted as a failure
- [x] The guest runs past frame 60 with **no host connected**, and keeps
      running
- [x] The trace log keeps growing while the emulator runs
- [x] Port 5002 is still reachable from the host when a host *does* connect
- [x] `pollstat.log` reports `afail=0` while idle

## Evidence after the fix

```
== port 5002 while running ==   OPEN
== port 5000 (ftp) ==           OPEN
== trace size ==                182 lines at t=30s, 412 lines at t=60s
== pollstat ==
  calls=120 accepts=0 afail=0 aerr=0 feeds=0 rbytes=0
```

`accepts=0` with `afail=0`: the poll ran 120 times, no client connected, and
no error was recorded — which is what a non-blocking listener should look
like.

## Notes

- The port's frame rate under the emulator is ~2.2 fps, against the ~1.59 fps
  the port quotes for hardware. The emulator is not the bottleneck.
- While measuring this, the core's audio backlog was observed growing
  (~7461 stereo frames pending and rising). That is the throttle from T025
  step 2 draining 512 frames/frame against a core producing ~534/frame — it
  is the intended behaviour of that change, but the backlog is unbounded and
  belongs to T025, not here.
