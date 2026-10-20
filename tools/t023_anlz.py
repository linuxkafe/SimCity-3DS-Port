"""T023 anlz toggle: measure frame cost with anlz on vs off.

The `anlz` verb toggles the static recompilation's analyze pass (SHA-256
frame hashing). T023 needs to confirm the toggle actually changes the cost,
because a harness that measures a toggle that does nothing is measuring
noise. Uses the corrected decimal parser.
"""
import argparse, statistics, sys, time
sys.path.insert(0, 'tools')
from aes_prof import Bridge, sample

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--pairs", type=int, default=6)
    ap.add_argument("--settle", type=float, default=2.0)
    args = ap.parse_args()

    send = Bridge(args.host, 5002)
    info = send("info")
    print(f"bridge OK ({info[:70]})")

    A, B = [], []
    for i in range(args.pairs):
        for arm, verb, bucket in (("A", "anlz 1", A), ("B", "anlz 0", B)):
            reply = send(verb)
            if not reply.startswith("OK"):
                print(f"pair {i} arm {arm}: toggle {verb!r} rejected: {reply!r}",
                      file=sys.stderr)
                continue
            time.sleep(args.settle)
            st, raw = sample(send)
            if st is None:
                print(f"pair {i} arm {arm}: bad sample {raw[:80]!r}", file=sys.stderr)
                continue
            bucket.append(st.get("loop", 0))
        print(f"  pair {i}: A={A[-1] if A else '-'}  B={B[-1] if B else '-'}", flush=True)

    ma = statistics.median(A) if A else 0
    mb = statistics.median(B) if B else 0
    print(f"\nloop (microseconds):")
    print(f"  anlz 1: n={len(A)} median={ma:,.0f}us = {ma/1e6:.3f}s")
    print(f"  anlz 0: n={len(B)} median={mb:,.0f}us = {mb/1e6:.3f}s")
    if ma and mb:
        delta = ma - mb
        print(f"  delta = {delta:+,.0f}us ({100*delta/ma:+.1f}% of anlz 1)")
    return 0

if __name__ == "__main__":
    sys.exit(main())