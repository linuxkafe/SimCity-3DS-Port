#!/usr/bin/env python3
"""
Interleaved A/B profiler for the 3DS debug bridge (T023).

Why interleaved A/B and not "measure arm A, then rebuild, then measure arm
B": REQUIREMENTS.md measurement discipline. `core` drifts by up to 42% with
scene content, and this game auto-advances on its own -- it reaches a busier
city while you are measuring. Sequential arms therefore measure the city as
well as the change. Interleaving cancels a monotone drift to first order.

Why it verifies the toggle took effect before trusting any sample: patch 0006
exists because a harness sampled against a flag the compiler had cached and
discarded all 16 pairs on "flag did not switch", reporting an exit code and
no number. A harness that cannot confirm its own precondition is worse than
no harness.

Why it reads `state` and not `prof`: `prof` used to bind every phase after the
first to the wrong phase (fixed in dd098df). `state` formats its own line and
was always correct. Both are read here so a discrepancy is visible rather than
silently resolved.

Usage:
    python3 tools/aes_prof.py --host 10.0.0.130 \
        --a "anlz 1" --b "anlz 0" --pairs 8 --settle 2.5

    # no toggle: just sample the frame cost
    python3 tools/aes_prof.py --host 10.0.0.130 --pairs 6
"""

import argparse
import re
import socket
import statistics
import sys
import time

# 3DS system tick rate. NOT one tick per microsecond -- see dd098df.
TICK_HZ = 26827953

PHASES = ("adv", "core", "render", "hud", "audio", "dbg", "present", "loop")


class Bridge:
    """One command, one connection.

    The bridge tears the connection down after each command, so a persistent
    socket raises ConnectionResetError on the second use. Reconnecting per
    command is the only shape that works against it.
    """

    def __init__(self, host, port=5002, timeout=6.0):
        self.host, self.port, self.timeout = host, port, timeout

    def __call__(self, cmd):
        s = socket.socket()
        s.settimeout(self.timeout)
        try:
            s.connect((self.host, self.port))
            s.send((cmd + "\n").encode())
            data = b""
            end = time.time() + self.timeout
            while time.time() < end:
                try:
                    chunk = s.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                data += chunk
                # Every verb in this bridge replies with a single '\n'-ended
                # line. Stopping there keeps a verbose reply from stalling on
                # a second read that will never come.
                if data.endswith(b"\n") and len(data) > 4:
                    break
            return data.decode(errors="replace").strip()
        finally:
            s.close()


# Fields in `state` that are NOT numeric. `build` is "ftp16" (contains 'p'),
# `src` is a pointer printed as 0x..., `fh` is a 16-char hex digest, and
# `last_error` is a free-form string. Trying int() on any of these raises
# ValueError, which the old int(v,16) consumed as "non-hex" -- but the real
# bug was that %u prints DECIMAL, not hex, so int(v,16) was wrong for every
# numeric field too (frame=72 → 114, instructions=936678 → 9672440).
NON_NUMERIC = {"build", "src", "fh", "last_error"}


def parse_state(reply):
    """`state` emits key=value pairs. Numeric fields are printed with %u/%llu
    (decimal), NOT hex. Only `input`/`injected` carry an explicit 0x prefix."""
    out = {}
    for k, v in re.findall(r"(\w+)=(\w+)", reply):
        if k in NON_NUMERIC:
            continue
        try:
            out[k] = int(v)
        except ValueError:
            pass  # skip anything we cannot parse rather than abort the sample
    if not out:
        raise ValueError("unparseable state reply: %r" % reply[:120])
    return out


def sample(send):
    """One `state` read. Returns None if the bridge answered something other
    than OK -- an ERR is a missed sample, not a zero, and averaging a zero in
    would silently bias the result low."""
    reply = send("state")
    if not reply.startswith("OK "):
        return None, reply
    try:
        return parse_state(reply), reply
    except ValueError:
        return None, reply


def report(label, values):
    if not values:
        print(f"  {label}: no samples")
        return None
    med = statistics.median(values)
    print(f"  {label}: n={len(values):>2}  median={med:>10,.0f}us "
          f"= {med / 1e6:>7.3f}s  = {1e6 / med:>6.2f} fps")
    return med


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--pairs", type=int, default=6)
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds to let the game reach the new arm's steady state")
    ap.add_argument("--a", help="verb+arg that selects arm A")
    ap.add_argument("--b", help="verb+arg that selects arm B")
    ap.add_argument("--field", default="loop",
                    help="state field to compare (default: loop)")
    args = ap.parse_args()

    send = Bridge(args.host, args.port)

    # Fail fast and loudly rather than sampling an emulator that is gone.
    info = send("info")
    if not info.startswith("OK "):
        print(f"bridge not answering on {args.host}:{args.port}: {info!r}",
              file=sys.stderr)
        return 2
    build = re.search(r"build=(\S+)", info)
    print(f"bridge OK  ({info[:70]})")
    if build:
        print(f"  build = {build.group(1)}")

    A, B, rejected = [], [], 0
    phases_seen = {}

    for i in range(args.pairs):
        for arm, verb, bucket in (("A", args.a, A), ("B", args.b, B)):
            if verb:
                reply = send(verb)
                # Confirm the precondition before trusting what follows.
                if not reply.startswith("OK"):
                    print(f"pair {i} arm {arm}: toggle {verb!r} rejected: {reply!r}",
                          file=sys.stderr)
                    rejected += 1
                    continue
            time.sleep(args.settle)

            st, raw = sample(send)
            if st is None:
                rejected += 1
                print(f"pair {i} arm {arm}: bad sample: {raw[:80]!r}",
                      file=sys.stderr)
                continue

            if args.field not in st:
                print(f"pair {i} arm {arm}: no field {args.field!r} in state",
                      file=sys.stderr)
                rejected += 1
                continue

            bucket.append(st[args.field])
            phases_seen.setdefault("loop", []).append(st.get("loop", 0))

        print(f"  pair {i}: A={A[-1] if A else '-'}  B={B[-1] if B else '-'}", flush=True)

    print(f"\n{args.field} (microseconds; {args.pairs} pairs, {rejected} rejected)")
    ma = report("A " + (args.a or "(no toggle)"), A)
    mb = report("B " + (args.b or "(no toggle)"), B)

    if ma and mb and ma != mb:
        delta = ma - mb
        print(f"\ndelta A-B = {delta:+,.0f}us/frame "
              f"({100 * delta / ma:+.1f}% of A)")
        print(f"B is {ma / mb:.2f}x {'faster' if mb < ma else 'slower'} than A")

    if len(phases_seen.get("loop", [])) > 1:
        loops = phases_seen["loop"]
        spread = 100 * (max(loops) - min(loops)) / max(1, min(loops))
        print(f"\nloop spread across all samples: {spread:.1f}%  "
              f"(min={min(loops):,} max={max(loops):,})")
        if spread > 60:
            print("  WARN: spread this wide means scene content is drifting the")
            print("        measurement. Shorten --settle and raise --pairs, or")
            print("        the A/B is not isolating the toggle.")

    return 0 if not rejected else 1


if __name__ == "__main__":
    sys.exit(main())