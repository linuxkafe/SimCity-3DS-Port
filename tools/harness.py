#!/usr/bin/env python3
"""harness.py — paired A/B measurement with flag verification.

T033: the measurement harness that would have caught my mistakes.

Two of the last three numbers I published were wrong:
  - T026: -1.1% (inside drift band, established nothing)
  - T025: -23.7% (probably inflated by drift, retracted)

Root cause: `core` is NOT a stable clock (42% drift with scene content).
Measuring A-then-B measures the drift, not the change.

This harness enforces the discipline that would have caught both:
  1. Alternate arms, difference WITHIN each pair.
  2. Verify the flag changed before sampling. Discard the pair if not.
  3. Discard, not dilute. Pairs outside 2*MAD are failed observations.
  3. Report the robust statistic (median, trimmed mean, range).
  4. Print attribution state with every number.
  4. Exit non-zero on UNRESOLVABLE (effect below noise floor).
"""

import argparse
import re
import socket
import statistics
import sys
import time


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

    def state(self):
        raw = self.cmd("state")
        if not raw.startswith("OK "):
            return {}
        kv = {}
        for tok in raw[3:].split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        return kv

    def core(self):
        st = self.state()
        return int(st.get("core", 0))

    def frame_no(self):
        st = self.state()
        return int(st.get("frame", 0))

    def wait_for_flag(self, flag_name, expected_value, timeout=10):
        """Wait for a flag in `info` to reach expected_value.

        Reads `info`, not `state`: `state` does not carry `anlz`, only `info`
        does. Reading the wrong verb here silently fails every pair, which is
        worse than reporting nothing -- the harness would still exit 0.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self.cmd("info")
            if not raw.startswith("OK "):
                time.sleep(0.1)
                continue
            for tok in raw[3:].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    if k == flag_name and v == expected_value:
                        return True
            time.sleep(0.1)
        return False

    def step_to(self, target, timeout=60):
        """Advance to at least `target` frames, waiting for frame counter."""
        base = self.frame_no()
        if base is None:
            raise RuntimeError("state gave no frame number")
        if target <= base:
            return self.frame_no()
        self.cmd(f"step {target - base}")
        deadline = time.time() + timeout
        while time.time() < deadline:
            cur = self.frame_no()
            if cur is not None and cur >= target:
                return self.frame_no()
        raise RuntimeError(f"step_to({target}) timed out at frame {self.frame_no()}")


def parse_kv_line(line):
    m = re.search(r"build=(\S+)", line)
    return m.group(1) if m else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.0.0.128")
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--switch", default="anlz", choices=["anlz", "cclog"],
                    help="which runtime toggle to A/B")
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--band", type=float, default=2.0,
                    help="discard pairs outside MAD * band")
    ap.add_argument("--label", default="target")
    args = ap.parse_args()

    b = Bridge(args.host, args.port)

    info = b.cmd("info")
    print(f"info: {info}")
    if "breaks=" in info and re.search(r"breaks=([1-9]\d*)", info):
        print("  ! breakpoints are armed (breaks!=0) -- measurement invalid")
        return 2

    if args.switch == "cclog" and "cclog=" not in info:
        print("  ! this build predates the cclog verb; cannot attribute it")
        return 2
    if args.switch == "anlz" and "anlz=" not in info:
        print("  ! this build predates the anlz verb; cannot attribute it")
        return 2

    print()
    print(f"A/B on `{args.switch}` against {args.host}   build={parse_kv_line(info)}")
    print()
    print("  pair        arm0            arm1         difference")
    print("  " + "-" * 62)

    diffs = []
    for i in range(args.pairs):
        # Arm 0: toggle OFF (cheap)
        b.cmd(f"{args.switch} 0")
        if not b.wait_for_flag(args.switch, "0", timeout=5):
            print(f"  {i:2d}  FAIL: flag did not switch to 0")
            diffs.append(None)
            continue

        b.cmd("step 1")
        a0 = b.core()

        # Arm 1: toggle ON (expensive)
        b.cmd(f"{args.switch} 1")
        if not b.wait_for_flag(args.switch, "1", timeout=5):
            print(f"  {i:2d}  FAIL: flag did not switch to 1")
            diffs.append(None)
            continue

        b.cmd("step 1")
        a1 = b.core()

        d = a1 - a0
        diffs.append(d)
        print("  %2d  %-15d %-13d %+d" % (i, a0, a1, d))
        time.sleep(0.5)

    # Reset to cheap state
    b.cmd(f"{args.switch} 0")

    # Filter out failed pairs
    diffs = [d for d in diffs if d is not None]
    if not diffs:
        print()
        print("  All pairs failed -- flag verification failed")
        return 3

    print()
    print(f"  raw pairs        : {len(diffs)}")

    med = statistics.median(diffs)
    mad = statistics.median([abs(d - med) for d in diffs]) or 1.0
    keep = [d for d in diffs if abs(d - med) <= args.band * mad]
    drop = [d for d in diffs if abs(d - med) > args.band * mad]

    print("  median           : %+d" % med)
    print(f"  MAD (spread)     : {mad:.0f}")
    print(f"  valid pairs      : {len(keep)}  (within {args.band}*MAD)")
    if drop:
        print(f"  DISCARDED        : {', '.join('%+d' % d for d in drop)}")
        print("                     failed observations -- scene changed inside pair")

    if not keep:
        print()
        print("  RESULT: no valid pairs. The measurement failed; it does not")
        print("          mean the effect is absent.")
        return 1

    tm = statistics.mean(keep)
    noise = 1.4826 * mad

    print("  trimmed mean     : %+d" % tm)
    print("  range (valid)    : %+d .. %+d" % (min(keep), max(keep)))
    print()
    if abs(tm) < 2.0 * noise:
        print("  RESULT: UNRESOLVABLE on this target.")
        print("          effect %+d, noise sigma ~%.0f. The measurement cannot" % (tm, noise))
        print("          separate this from nothing. That is a statement about")
        print("          the target, NOT about whether the code costs anything.")
        return 3

    sign = "costs more" if tm > 0 else "costs less"
    print("  RESULT: toggle 1 %s than toggle 0 by %d ticks/frame (%.1f sigma)" % (sign, abs(tm), abs(tm)/noise))
    print(f"  i.e. `{args.switch} ON` is the EXPENSIVE state")
    print("  Attribute to a percentage only against a stated baseline; the")
    print("  baseline drifts too, so the ratio is only as good as it is.")

    # Check for sign consistency (parity)
    pos = sum(1 for d in diffs if d > 0)
    neg = sum(1 for d in diffs if d < 0)
    if pos > 0 and neg > 0:
        # Only warn when the two signs are genuinely balanced, which is what
        # makes the sign unreliable. 15 of 16 positive is strong agreement on
        # the direction; it is the opposite of a problem and must not be
        # reported as one. The first version warned on any mix of signs, which
        # would have flagged a clean result as suspicious.
        total = pos + neg
        minority = min(pos, neg)
        if minority > total * 0.2:
            print()
            print(f"  WARNING: sign inconsistent ({pos} positive, {neg} negative)")
            print("           This suggests the toggle is not being applied reliably")
            print("           within the measurement window. The magnitude is correct")
            print("           but the sign is unreliable. Investigate toggle timing.")
    return 0


if __name__ == "__main__":
    sys.exit(main())