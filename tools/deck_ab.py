#!/usr/bin/env python3
"""deck_ab.py — paired A/B measurement against the SimCity port.

Tal como `netdbg.py`/`netfb.py`, speak to the debug port. This one exists
because measuring by hand got two of the last three published numbers wrong,
in different ways:

  * T026 reported -1.1%, which was inside the noise. The frontend render phase
    is 3.3% of the frame, so it could not have been large -- but the
    measurement did not establish that, and I wrote it up as a result anyway.
  * T025 reported -23.7%, which the drift analysis later suggested was
    inflated. Drift alone moves 50M ticks against a reported 46M effect.

The root cause of both is the same: `core` is NOT a stable clock. It varies by
up to 42% with scene content as the demo camera moves and sprite load changes.
Measuring arm A and then arm B, minutes apart, measures the drift.

So: alternate the arms, difference WITHIN each pair, and treat a pair outside
the expected band as a failed observation rather than something to dilute into
a mean. The last manual run had 2 of 12 pairs contaminated (+72.6M and -25.0M);
averaging those in would have produced a confident and wrong number.

Usage:
    deck_ab.py --host 10.0.0.128 --switch cclog --pairs 12
    deck_ab.py --host 10.0.0.128 --switch anlz  --pairs 12
    deck_ab.py --host 10.0.0.130 --switch cclog --pairs 12 --label device
"""

import argparse
import re
import socket
import statistics
import sys
import time


class Bridge:
    """The debug port accepts ONE client at a time and frees it on the next
    poll, and a frame takes ~0.75s, so every command has to be spaced or it
    collides with the previous socket."""

    def __init__(self, host, port, timeout=40, settle=2.2):
        self.host, self.port = host, port
        self.timeout, self.settle = timeout, settle

    def cmd(self, line):
        time.sleep(self.settle)
        s = socket.create_connection((self.host, self.port),
                                     timeout=self.timeout)
        s.settimeout(self.timeout)
        try:
            f = s.makefile("rwb")
            f.write((line + "\n").encode())
            f.flush()
            return f.readline().decode().strip()
        finally:
            try:
                s.close()
            except OSError:
                pass

    def kv(self):
        """state -> dict, or None if a field is missing."""
        raw = self.cmd("state")
        if not raw.startswith("OK "):
            return None
        out = {}
        for tok in raw[3:].split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                out[k] = v
        return out

    def core(self):
        st = self.kv()
        if not st or "core" not in st:
            raise RuntimeError("state gave no core field: %r" % (st,))
        return int(st["core"])


def parse_kv_line(line):
    m = re.search(r"build=(\S+)", line)
    return m.group(1) if m else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.0.0.128")
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--switch", default="cclog", choices=["cclog", "anlz"],
                    help="which runtime toggle to A/B")
    ap.add_argument("--pairs", type=int, default=12)
    ap.add_argument("--band", type=float, default=0.45,
                    help="reject a pair outside the median by this fraction")
    ap.add_argument("--label", default="target")
    args = ap.parse_args()

    b = Bridge(args.host, args.port)
    info = b.cmd("info")
    print("info: %s" % info)

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
    print("A/B on `%s` against %s   build=%s"
          % (args.switch, args.host, parse_kv_line(info)))
    print()
    print("  pair        arm0            arm1         difference")
    print("  " + "-" * 62)

    diffs = []
    for i in range(args.pairs):
        b.cmd("%s 0" % args.switch)          # arm 0 = toggle OFF
        a0 = b.core()
        b.cmd("%s 1" % args.switch)          # arm 1 = toggle ON
        a1 = b.core()
        d = a1 - a0
        diffs.append(d)
        print("  %3d  %-15d %-13d %+d" % (i, a0, a1, d))
        time.sleep(0.5)

    b.cmd("%s 0" % args.switch)              # leave in the cheap state
    print()
    print("  raw pairs        : %d" % len(diffs))

    # Band from the median ABSOLUTE DEVIATION, not from the median.
    #
    # The first version used the median for the band and it was wrong in an
    # instructive way: on the emulated target the effect (~0.75M) is smaller
    # than the within-pair spread (~1M), so the median lands near zero, the
    # band collapses to almost nothing, and all 12 pairs get discarded. The
    # script then reports "no valid pairs", which reads like "the effect is
    # absent" when the truth is "the effect is below the noise floor".
    #
    # MAD is the right scale here precisely because it measures spread and is
    # unmoved by the effect itself. And when |effect| is not clearly larger
    # than the noise, the answer is UNRESOLVABLE, not zero.
    med = statistics.median(diffs)
    mad = statistics.median([abs(d - med) for d in diffs]) or 1.0
    keep = [d for d in diffs if abs(d - med) <= 2.0 * mad]
    drop = [d for d in diffs if abs(d - med) > 2.0 * mad]

    print("  raw pairs        : %d" % len(diffs))
    print("  median           : %+d" % med)
    print("  MAD (spread)     : %.0f" % mad)
    print("  valid pairs      : %d  (within 2*MAD)" % len(keep))
    if drop:
        print("  DISCARDED        : %s" % ", ".join("%+d" % d for d in drop))
        print("                     failed observations -- not outliers to")
        print("                     dilute; the scene moved inside the pair")

    noise = 1.4826 * mad          # MAD -> sigma for a normal distribution
    tm = statistics.mean(keep) if keep else med
    print("  trimmed mean     : %+d" % tm)
    if keep:
        print("  range (valid)    : %+d .. %+d" % (min(keep), max(keep)))
    print()
    if abs(tm) < 2.0 * noise:
        print("  RESULT: UNRESOLVABLE on this target.")
        print("          effect %+d, noise sigma ~%.0f. The measurement cannot"
              % (tm, noise))
        print("          separate this from nothing. That is a statement about")
        print("          the target, NOT about whether the code costs anything.")
        print("          Ticks in an emulator are not the same clock as ticks on")
        print("          hardware; use this target for functional tests and")
        print("          re-measure performance on the device.")
        return 3

    sign = "costs more" if tm > 0 else "costs less"
    print("  RESULT: toggle 1 %s than toggle 0 by %d ticks/frame (%.1f sigma)"
          % (sign, abs(tm), abs(tm) / noise))
    print("  i.e. %s" % ("%s ON is the EXPENSIVE state" % args.switch
                         if tm > 0 else
                         "%s ON is the CHEAP state" % args.switch))
    print("  Attribute to a percentage only against a stated baseline; the")
    print("  baseline drifts too, so the ratio is only as good as it is.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
