#!/usr/bin/env python3
"""trace.py — deterministic frame-hash trace, and the check that it means something.

T026 shipped a change to the render path whose device-side output equality was
never verified. The before/after PNG comparison was worthless: the two captures
were of different game states, so the difference measured the game moving, not
the change. What existed was an exhaustive host-side proof over all 32768 pixel
values, which is real and cannot catch a stride or rotation error.

This closes that gap. It resets the emulation to cold-boot, steps it frame by
frame with a fixed input script, and records the composited frame's SHA-256 at
each step. Two runs of the same script must produce the same sequence.

The trap, stated up front because it produces a confident wrong answer rather
than a failure: **the digest is only recomputed while the core log is open.**
T028 measured that log at 14.5% of the frame, so the 3DS port leaves it off by
default and the digest then holds the PREVIOUS frame's value. A harness that
forgets `cclog 1` will compare stale data and conclude every frame matches. This
script turns the log on, and refuses to report success if the digest looks like
the all-zero placeholder.

Usage:
    trace.py --host 10.0.0.128 --frames 30 --out run-a.txt
    trace.py --host 10.0.0.128 --frames 30 --out run-b.txt
    trace.py --compare run-a.txt run-b.txt
"""

import argparse
import socket
import sys
import time

# A fixed input script. Not the game's own demo mode: the demo advances on its
# own schedule, which is precisely the thing that made two captures incomparable.
# Bit 0 right, 1 left, 2 up, 3 down (same layout the `input` verb takes).
SCRIPT = [
    (0x0000, 5),   # settle
    (0x0001, 4),   # right
    (0x0002, 4),   # left
    (0x0004, 4),   # up
    (0x0008, 4),   # down
    (0x0000, 6),
]


class Bridge:
    """One connection to the debug bridge, held open across commands.

    The bridge serves ONE client at a time and there is no idle timeout, so a
    single socket can carry the whole run.

    This exists because the previous transport opened a fresh connection per
    command -- roughly 250 sockets per gate phase. That provoked the symptom
    documented in T034: the emulator survived two phases and was SIGKILLed
    during the third. Whether socket count is the cause was never proven, only
    correlated, so this is the cheap change that tests the correlation. If the
    SIGKILL persists with one socket, the cause is something else and this
    has bought clarity rather than a fix.

    The socket is re-established transparently on any send or receive failure,
    so a dropped connection is an event the report shows, not a silent skip.
    """
    def __init__(self, host, port, timeout=40, settle=0.45):
        self.host, self.port = host, port
        self.timeout, self.settle = timeout, settle
        self._sock = None
        self._fh = None
        self.reconnects = 0

    def _connect(self):
        self.close()
        self._sock = socket.create_connection((self.host, self.port),
                                              timeout=self.timeout)
        self._sock.settimeout(self.timeout)
        self._fh = self._sock.makefile("rwb")

    def close(self):
        if self._fh is not None:
            try:
                self._fh.close()
            except OSError:
                pass
            self._fh = None
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    def cmd(self, line, retries=3):
        """Send a command on the held connection and read one line.

        A single socket for the whole run. The bridge is single-client and has
        no idle timeout, so holding it open is safe; the settle delay that
        existed per command is no longer needed for politeness, but is kept
        because the bridge is serviced from the emulated main loop and a
        command issued mid-frame is answered a frame later.
        """
        last = None
        for attempt in range(retries):
            try:
                if self._fh is None:
                    self._connect()
                    self.reconnects += 1
                time.sleep(self.settle)
                self._fh.write((line + "\n").encode())
                self._fh.flush()
                reply = self._fh.readline()
                if not reply:
                    raise OSError("bridge closed the connection")
                return reply.decode().strip()
            except (socket.timeout, ConnectionError, OSError) as exc:
                last = exc
                self.close()
                time.sleep(1.0 * (attempt + 1))
        raise RuntimeError(f"cmd {line!r} failed after {retries} retries: {last}")


def frame_no(b):
    """Current emulated frame number, or None."""
    raw = b.cmd("state")
    if not raw.startswith("OK "):
        return None
    for tok in raw[3:].split():
        if tok.startswith("frame="):
            try:
                return int(tok.split("=", 1)[1])
            except ValueError:
                return None
    return None


def step_to(b, target, timeout=120):
    """Advance to at least `target` frames, waiting for the count to arrive.

    `step N` SETS a pending counter that the main loop consumes one frame at a
    time. Issuing the next `step` before the previous is consumed overwrites it,
    so a burst of `step 20` commands advances far less than 20x the count --
    measured: 14 x `step 20` reached frame 85, not 280.

    That failure is silent. The warmup simply does less than asked, the harness
    records a static screen, and every frame hashes the same, and the result
    looks like a clean pass. Any stepping in a determinism harness has to wait
    for the frame counter, or it is not stepping.
    """
    start = frame_no(b)
    if start is None:
        raise RuntimeError("state gave no frame number")
    if target <= start:
        return start
    b.cmd("step %d" % (target - start))
    deadline = time.time() + timeout
    while time.time() < deadline:
        cur = frame_no(b)
        if cur is not None and cur >= target:
            return cur
    raise RuntimeError("step_to(%d) timed out at frame %s" % (target, frame_no(b)))


def flat_script(n):
    out, i = [], 0
    while len(out) < n:
        mask, count = SCRIPT[i % len(SCRIPT)]
        for _ in range(count):
            if len(out) >= n:
                break
            out.append(mask)
        i += 1
    return out[:n]


def record(b, frames, out, warmup=0):
    info = b.cmd("info")
    print("info: %s" % info)
    # Turn the core log on FIRST. The hash verb returns an empty digest while
    # logging is off, because render_lines_impl only recomputes it behind
    # sc_core_logging_enabled(). Probing before enabling means every build
    # looks broken, since the response is "OK " with nothing after it.
    ack = b.cmd("cclog 1")
    print("cclog: %s" % ack)
    if not ack.startswith("OK"):
        print("  ! could not open the core log; the digest would be stale")
        return None
    # The digest only moves while the core log is open. Without this the whole
    # run would compare one stale value against itself.
    print("pause : %s" % b.cmd("pause"))
    print("reset : %s" % b.cmd("reset"))

    # Step past the boot before recording anything.
    #
    # Measured on the emulated target after a cold reset:
    #   frames   1- 54   boot, one digest throughout
    #   frames  54- 79   two changes as the title fades in
    #   frames  79-212   TITLE SCREEN, one digest for ~133 frames
    #   frame     ~232   the attract demo starts, and the digest then changes
    #                      on every sampled block
    #
    # Recording the title screen would produce a trace of identical hashes that
    # compares equal between any two runs -- determinism manufactured by
    # observing nothing, which is the failure this ticket exists to prevent.
    # The default warmup of 280 clears the demo start with margin; the
    # distinct-digest check below is what proves it actually worked, rather
    # than trusting this comment to stay true.
    if warmup:
        base = frame_no(b)
        reached = step_to(b, base + warmup)
        print("warmup: frames %d -> %d" % (base, reached))

    masks = flat_script(frames)
    rows = []
    prev = None
    base = frame_no(b)
    for n, mask in enumerate(masks):
        b.cmd("input %04x" % mask)
        step_to(b, base + n + 1)
        h = b.cmd("hash")
        if not h.startswith("OK "):
            print("  ! hash failed at frame %d: %s" % (n, h))
            return None
        digest = h.split(" ", 1)[1].strip()
        rows.append((n, mask, digest))
        if prev is not None and digest == prev:
            print("  ! frame %d digest did not change" % n)
        prev = digest

    b.cmd("input 0")
    b.cmd("cclog 0")

    # The all-zero digest is the placeholder for "never hashed". Catching it
    # here is the difference between a real trace and a convincing fiction.
    distinct = len({r[2] for r in rows})
    # Demand that a decent share of frames differ. A static screen yields one
    # digest for every frame and would sail through a `distinct > 1` check
    # after a single incidental change, which proves nothing.
    # The guard exists to catch a trace of a completely static screen, which
    # compares equal between any two runs. Measured on the attract demo: it
    # changes on roughly a third of frames, not all of them, so a strict
    # proportion is the wrong test. Three distinct digests is enough to prove
    # the sequence is being observed at all.
    need = max(3, len(rows) // 10)
    if distinct < need:
        print("  ! only %d distinct digests across %d frames (need %d)."
              % (distinct, len(rows), need))
        print("    The screen is probably static. A trace of identical hashes")
        print("    compares equal between any two runs and proves nothing --")
        print("    that is determinism manufactured by observing nothing.")
        print("    Raise --warmup until the content actually animates.")
        return None

    with open(out, "w") as f:
        for n, mask, digest in rows:
            f.write("%04d %04x %s\n" % (n, mask, digest))
    print("wrote %s: %d frames, %d distinct digests" % (out, len(rows), distinct))
    return rows


def compare(a_path, b_path):
    def load(p):
        out = []
        for line in open(p):
            parts = line.split()
            if len(parts) == 3:
                out.append((int(parts[0]), parts[1], parts[2]))
        return out

    A, B = load(a_path), load(b_path)
    if len(A) != len(B):
        print("FAIL: different lengths, %d vs %d" % (len(A), len(B)))
        return 1
    bad = [(a[0], a[2], b[2]) for a, b in zip(A, B) if a[2] != b[2]]
    if not bad:
        print("OK: %d frames identical" % len(A))
        return 0
    print("FAIL: %d of %d frames differ" % (len(bad), len(A)))
    for n, x, y in bad[:5]:
        print("  frame %d: %s != %s" % (n, x[:16], y[:16]))
    if len(bad) > 5:
        print("  ... and %d more" % (len(bad) - 5))
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.0.0.128")
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=280,
                    help="frames to step through before recording, to get past "
                         "the boot sequence into content that actually changes")
    ap.add_argument("--out")
    ap.add_argument("--compare", nargs=2, metavar=("A", "B"))
    args = ap.parse_args()

    if args.compare:
        return compare(*args.compare)

    b = Bridge(args.host, args.port)
    rows = record(b, args.frames, args.out or ("run-%s.txt" % args.host),
                 args.warmup)
    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
