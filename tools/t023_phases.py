"""T023 phase breakdown: per-phase microseconds, no toggle, 8 pairs.

The `state` verb reports all 8 phases in one line. This script reads them
all and prints a per-phase median so we can see which phase dominates the
4-second frame and which are noise.

Usage:
    python3 tools/t023_phases.py --host 10.0.0.128 --pairs 8
"""
import argparse
import statistics
import sys
import time
sys.path.insert(0, 'tools')
from aes_prof import Bridge, sample

PHASES = ("adv", "core", "render", "hud", "audio", "dbg", "present", "loop")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--pairs", type=int, default=8)
    ap.add_argument("--settle", type=float, default=2.0)
    args = ap.parse_args()

    send = Bridge(args.host, args.port)
    info = send("info")
    print(f"bridge OK ({info[:70]})")

    per_phase = {p: [] for p in PHASES}
    rejected = 0

    for i in range(args.pairs):
        time.sleep(args.settle)
        st, raw = sample(send)
        if st is None:
            rejected += 1
            print(f"pair {i}: bad sample {raw[:80]!r}", file=sys.stderr)
            continue
        for p in PHASES:
            if p in st:
                per_phase[p].append(st[p])
        print(f"  pair {i}: loop={st.get('loop', '?')} core={st.get('core', '?')} "
              f"adv={st.get('adv', '?')} render={st.get('render', '?')} "
              f"dbg={st.get('dbg', '?')} present={st.get('present', '?')} "
              f"audio={st.get('audio', '?')} hud={st.get('hud', '?')}",
              flush=True)

    print(f"\nPer-phase median (microseconds; {args.pairs} pairs, {rejected} rejected):")
    print(f"{'phase':>10} {'median':>12} {'ms':>8} {'% of loop':>10}")
    loop_med = statistics.median(per_phase["loop"]) if per_phase["loop"] else 0
    for p in PHASES:
        vals = per_phase[p]
        if not vals:
            print(f"{p:>10}  no samples")
            continue
        med = statistics.median(vals)
        pct = 100 * med / loop_med if loop_med else 0
        print(f"{p:>10} {med:>12,.0f} {med/1000:>8.1f} {pct:>9.1f}%")

    # Spread check on loop
    if len(per_phase["loop"]) > 1:
        spread = 100 * (max(per_phase["loop"]) - min(per_phase["loop"])) / max(1, min(per_phase["loop"]))
        print(f"\nloop spread: {spread:.1f}%  (min={min(per_phase['loop']):,} max={max(per_phase['loop']):,})")
        if spread > 60:
            print("  WARN: scene content is drifting the measurement.")

    return 0 if not rejected else 1

if __name__ == "__main__":
    sys.exit(main())