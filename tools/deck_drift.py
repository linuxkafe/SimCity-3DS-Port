#!/usr/bin/env python3
"""deck_drift.py — measure whether the Deck can currently measure anything.

T035 was blocked by its own control. The Oct-18 binary measured
`create rc=1` + 5 rendered advances at 10:31 and **0 advances** at 11:45 — same
Deck, same binary, same ROM, same emulator. Four builds were then run in
sequence with no control between them, so a mid-session environmental shift was
free to look like a per-arm difference.

`deck_ab.py` solves the equivalent problem for the debug port by pairing arms.
That fix does not transfer here: every build currently dies at `boot:`, so the
port never opens and there is no toggle to flip. The control has to be a
*binary* and the transport has to be ssh + the files a run leaves behind.

Two design choices are deliberate and were forced by the failure analysis in
aes/tickets/T036-drift-detecting-harness.md:

1. **Each run is reduced to a categorical stop signature, not a number.**
   Copying deck_ab.py's median-absolute-deviation band onto `advances` counts
   would collapse: those counts are small integers whose MAD is degenerate, and
   deck_ab.py already documents that trap happening with tick counts. A
   categorical signature has no band to collapse.

2. **The control series is reported before any arm is ranked.** The primary
   question is not "which arm is better" but "is this target in a state where any
   measurement is valid". A tool that ranks arms first has that backwards.

The tool never claims to explain the drift. It claims only that drift becomes
visible.

Exit codes:
    0  control stable, arms ranked
    2  preflight or integrity refusal (binary digest mismatch, missing file)
    3  UNSTABLE CONTROL — arms deliberately NOT ranked
    4  inconclusive (too few samples to say anything)

Usage:
    deck_drift.py --control ~/simcity-test/simcity_test.3dsx \\
                  --arm arm-214c77c.3dsx --reps 3
    deck_drift.py --control c.3dsx --arm a.3dsx --reps 3 --dry-run
"""

import argparse
import re
import subprocess
import sys
import time


# Built at runtime so that this script's own remote command line never contains
# the literal pattern. A plain pgrep/pkill -f "azahar.AppImage" matches the
# shell that is running the pgrep, because the shell's command line contains that
# string -- which killed an ssh mid-session on 2026-10-19 and silently skipped
# the `cp` that followed it.
ROMDIR = ("/run/media/deck/mmcblk0p1/Emulation"
          "/storage/azahar/sdmc")
TRACE = ROMDIR + "/simcity-trace.log"
AZLOG = "/home/deck/.local/share/azahar-emu/log/azahar_log.txt"


def ssh(host, script, timeout):
    """One ssh round trip. Kept to a single round trip per run so the observer
    perturbs the target as little as possible."""
    p = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host,
         script],
        capture_output=True, text=True, timeout=timeout)
    return p.stdout, p.returncode


def kv(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def one_run(host, label, remote_bin, launch_s, timeout):
    """Launch one binary, then classify where it stopped.

    Returns (signature_dict, remote_md5). Never returns on a digest mismatch --
    that is an integrity refusal, not a measurement.
    """
    # For the control, use the original file directly (no copy) -- copying to
    # a different filename broke Azahar's title ID detection. Arms still use
    # a scratch copy since they come from dev host.
    expect = remote_bin["md5"]
    src = remote_bin["path"]
    if label == "control":
        scratch = src  # use original directly
    else:
        scratch = "/run/media/deck/mmcblk0p1/Emulation/roms/n3ds/_drift_%s.cxi" % label

# Minimal pre-run: NO kill. Any pkill over ssh matches the remote shell's
    # own command line and suicides the run, leaving `no-trace`. The manual run
    # that worked had NO pre-kill. Just a tiny settle wait.
    RESET = (
        'sleep 0.5;'
    )

    # The kill pattern is assembled REMOTELY, in two steps, so the literal string
    # never appears in the remote shell's own command line. Inlining it as
    # pkill -f "azahar.AppImage" makes the remote shell match itself and die --
    # which silently skips every following step and reports a constant, obviously
    # wrong `no-trace` for every run. This is the second time this exact class of
    # bug has cost a run on this project.
    KILL = 'P="azahar"; P="$P.AppImage"; pkill -f "$P" 2>/dev/null; true;'

    pre = (
        'export PATH=/usr/bin:/bin:$PATH;'
        'cp -f "%s" "%s";'                      # (src, scratch)
        'echo "deck_md5=$(md5sum "%s" | cut -d" " -f1)";'
        'echo "want_md5=%s";'
        'rm -f "%s";'                            # trace
        '%s'                                     # full RESET (assembled remotely)
        'sleep 1;'
        'echo "load_before=$(cut -d" " -f1-3 /proc/loadavg)";'
        'date -Is'
    ) % (src, scratch, scratch, expect, TRACE, RESET)

    out, _ = ssh(host, pre, 60)
    got = kv(out)
    if got.get("deck_md5") != expect:
        # Integrity failure. Refuse rather than measure the wrong binary --
        # a stale binary produces a valid-looking wrong answer.
        return ({"sig": "INTEGRITY-MISMATCH",
                 "note": "deck=%s want=%s" % (got.get("deck_md5"), expect),
                 "advances": 0, "rendered": 0, "svc": 0, "md5": got.get("deck_md5")},
                got.get("deck_md5"))

    # `timeout -k` is mandatory, not defensive. run.sh `exec`s the AppImage, which
    # does not die on SIGTERM, so a bare `timeout N` returns only when the whole
    # ssh is torn down -- measured at >105 s for a 45 s bound on 2026-10-19.
    # Without the KILL grace the bounded run is not bounded.
    run = ('export PATH=/usr/bin:/bin:$PATH; cd /home/deck/simcity-test; '
           'timeout -k 10 %d ./run.sh "%s" >/dev/null 2>&1; '
           'echo "run_rc=$?"; date -Is' % (launch_s, scratch))
    rout, _ = ssh(host, run, launch_s + 90)
    rk = kv(rout).get("run_rc", "?")

    # Leave nothing running, then read the observables back.
    post = (
        'export PATH=/usr/bin:/bin:$PATH;'
        '%s'                                     # KILL, then settle
        'sleep 1;'
        'echo "advances=$(grep -c "advance #" "%s" 2>/dev/null || echo 0)";'
        'echo "rendered=$(grep -c "rendered=1" "%s" 2>/dev/null || echo 0)";'
        'echo "svc=$(grep -c "svc=0x32" "%s" 2>/dev/null || echo 0)";'
        'echo "trace_bytes=$(stat -c%%s "%s" 2>/dev/null || echo 0)";'
        'echo "last=$(tail -n 1 "%s" 2>/dev/null | tr -d "\\r")";'
        'rm -f "%s"'
    ) % (KILL, TRACE, TRACE, AZLOG, TRACE, TRACE, scratch if label != "control" else "/dev/null")

    out, _ = ssh(host, post, 90)
    f = kv(out)

    advances = int(f.get("advances", 0) or 0)
    rendered = int(f.get("rendered", 0) or 0)
    svc = int(f.get("svc", 0) or 0)
    last = f.get("last", "")
    tbytes = int(f.get("trace_bytes", 0) or 0)

    # Categorical stop signature. Order matters: the first stage reached wins,
    # because "how far did it get" is what changed, not how many frames.
    if tbytes == 0:
        stage = "no-trace"
    elif "create: rc=" not in last and advances == 0 and "create" not in last:
        stage = "after-boot"
    elif advances == 0:
        stage = "after-create"
    else:
        stage = "advanced"

    # The signature carries QUALITATIVE state only: how far the run got, and
    # whether it drew. It deliberately excludes `svc`, which is a RATE over a
    # fixed launch window -- measured 25,24,25,24 on four runs of one unchanged
    # binary. Including it made the first version report UNSTABLE_CONTROL on a
    # stable target. A stability test that cries wolf is worse than none.
    # Counts are still printed, and reported with their own spread.
    sig = "%s:rnd=%d" % (stage, rendered)
    return ({"sig": sig, "stage": stage, "advances": advances,
             "rendered": rendered, "svc": svc, "md5": expect, "last": last,
             "run_rc": rk},
            expect)


def digest(host, path, timeout=40):
    out, _ = ssh(host, 'md5sum "%s"' % path, timeout)
    t = out.split()
    return {"path": path, "md5": t[0] if t else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="deck@steamdeck")
    ap.add_argument("--control", required=True,
                    help="binary path on the Deck used as the control")
    ap.add_argument("--arm", action="append", default=[],
                    help="binary path on the Deck to compare (repeatable)")
    ap.add_argument("--reps", type=int, default=3,
                    help="control samples per arm (total runs = reps*2*arms+1)")
    ap.add_argument("--launch-s", type=int, default=90,
                    help="bounded launch duration per run")
    ap.add_argument("--gap-s", type=int, default=5,
                    help="settle between runs")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the protocol, launch nothing")
    args = ap.parse_args()

    if args.reps < 1:
        print("! --reps must be >= 1")
        return 2

    # Interleave so control samples are spread across the whole session in time.
    # The first version front-loaded reps*arms+1 controls and then alternated,
    # which wasted the leading samples bracketing nothing and left the opening
    # minutes unmonitored -- the opposite of what decision 3 in T036 is for.
    plan = ["control"]
    for i in range(len(args.arm)):
        for _ in range(args.reps):
            plan += ["arm%d" % i, "control"]

    print("=== drift-detection protocol ===")
    print("  host      : %s" % args.host)
    print("  control   : %s" % args.control)
    print("  arms      : %s" % (", ".join(args.arm) or "(none)"))
    print("  launch/run: %ds bounded, %ds gap" % (args.launch_s, args.gap_s))
    print("  sequence  : %s" % " ".join(plan))
    print("  total runs: %d (~%d min)"
          % (len(plan), len(plan) * (args.launch_s + args.gap_s) // 60 + 1))
    if args.dry_run:
        print("\n  --dry-run: nothing launched.")
        return 0

    ctl = digest(args.host, args.control)
    if not ctl["md5"]:
        print("! control not readable on %s -- refusing" % args.host)
        return 2
    arms = []
    for a in args.arm:
        d = digest(args.host, a)
        if not d["md5"]:
            print("! arm not readable on %s -- refusing" % a)
            return 2
        arms.append(d)
    print("  control md5: %s" % ctl["md5"])
    for i, a in enumerate(arms):
        print("  arm%d md5  : %s  (%s)" % (i, a["md5"], a["path"]))
    print()

    bins = {"control": ctl}
    for i, a in enumerate(arms):
        bins["arm%d" % i] = a

    print("  %-4s %-9s %-30s %5s %5s %5s" %
          ("#", "role", "signature", "adv", "rnd", "svc"))
    print("  " + "-" * 68)

    series = []
    for n, role in enumerate(plan):
        r, _ = one_run(args.host, role, bins[role], args.launch_s, 0)
        series.append((role, r))
        print("  %-4d %-9s %-30s %5d %5d %5d"
              % (n, role, r["sig"][:30], r["advances"], r["rendered"], r["svc"]))
        run_rc = r.get("run_rc", "?")
        # 137 = SIGKILL from `timeout -k`, 0 = clean exit. Anything else means the
        # launch itself failed and the signature below is not a measurement of the binary.
        if run_rc not in ("137", "0", "?"):
            print("       ! launch did not run: run_rc=%s" % run_rc)
        if r["sig"].startswith("INTEGRITY"):
            print("\n  ! integrity mismatch -- aborting. Measured nothing.")
            return 2
        if r["stage"] == "no-trace":
            print("       ! no trace written -- the launch produced nothing;")
            print("         this signature is NOT a measurement of the binary.")
        time.sleep(args.gap_s)

    # ---- Control stability is the primary result. ----
    ctl_sigs = [r["sig"] for role, r in series if role == "control"]
    print()
    print("  control series (%d samples):" % len(ctl_sigs))
    for i, s in enumerate(ctl_sigs):
        print("    %d. %s" % (i, s))
    distinct = sorted(set(ctl_sigs))
    stable = len(distinct) == 1

    if not stable:
        print()
        print("  RESULT: UNSTABLE_CONTROL")
        print("          The control produced %d different outcomes on the same"
              % len(distinct))
        print("          binary during one session. Arms are NOT ranked:")
        print("          with the endpoint moving, any arm difference is")
        print("          uninterpretable. This is the T035 condition.")
        print("          Do not report an arm comparison from this run.")
        return 3

    print("  control stable: %s" % distinct[0])
    if len(ctl_sigs) < 3:
        print()
        print("  RESULT: INCONCLUSIVE -- only %d control samples. Drift with a"
              % len(ctl_sigs))
        print("          period longer than this session would be invisible.")
        print("          Raise --reps before trusting a comparison.")
        return 4

    print()
    for i, a in enumerate(arms):
        vals = [r["sig"] for role, r in series if role == "arm%d" % i]
        uniq = sorted(set(vals))
        verdict = ("matches control" if uniq == distinct
                   else "DIFFERS: %s" % ", ".join(uniq))
        print("  arm%d (%s)" % (i, a["md5"]))
        for j, v in enumerate(vals):
            print("    %d. %s" % (j, v))
        print("    -> %s" % verdict)

    print()
    print("  RESULT: control stable across %d samples; ranking above is"
          % len(ctl_sigs))
    print("          interpreted against that stability only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
