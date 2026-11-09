#!/usr/bin/env bash
# Decompose the host frame rate into (guest work) + (emulator fixed overhead).
#
# The metric that has been used so far is `core`, the guest's emulated CPU
# timer per frame. It is NOT host time: svcGetSystemTick returns the emulated
# CPU timer, so `core` measures emulated cycles. The emulator's frames per
# second is host wall time. Nothing so far has established that the two are
# proportional. This script measures, in one boot and one game state:
#
#   arm  rskip  anlz  mode   what it removes
#   A    0      1     0      nothing (current baseline)
#   B    0      0     0      analyze() + per-frame SHA-256
#   C    15     0     0      all render work (BG/subscreen/sprites/BGRA)
#   D    2      0     0      BG layer evaluation only
#   E    0      0     1      host frame conversion (advance_headless)
#
# For each sample it records core/s, instructions/frame and host fps. Arms are
# interleaved and repeated so that scene drift (documented at up to 42 %)
# hits every arm equally instead of being confused for an arm effect.
#
# Then it fits host_ms_per_frame = a * (guest work) + overhead for both
# candidate work proxies (emulated cycles and executed instructions). The
# intercept is the emulator's fixed per-frame cost. 30 fps needs
# host_ms_per_frame <= 33.3 ms; if the intercept alone exceeds that, no
# amount of guest-side work makes 30 fps reachable on this emulator.
#
# Usage: decompose-fps.sh [3dsx-on-deck]
set -euo pipefail

DECK="${DECK:-deck@steamdeck}"
ROM="${1:-/home/deck/simcity-test/simcity_decompose.3dsx}"

if [ ! -f "${LOCAL_3DSX:-}" ]; then :; fi
if [ -n "${LOCAL_3DSX:-}" ]; then
    scp -q "$LOCAL_3DSX" "$DECK:$ROM"
    echo "== deployed $LOCAL_3DSX -> $ROM =="
fi

ssh -o BatchMode=yes "$DECK" bash -s -- "$ROM" <<'SCRIPT'
set -u
ROM="$1"
SDMC=/home/deck/.local/share/azahar-emu/sdmc
cd /home/deck/simcity-test
pkill -f "simcity-headless" 2>/dev/null || true
sleep 2
rm -f "$SDMC/simcity-trace.log"
timeout 600 ./simcity-headless --max-seconds 480 "$ROM" >/tmp/decompose.out 2>&1 &
sleep 25
python3 - <<'PY'
import socket, time, re, statistics

def verb(c, t=25):
    s = socket.create_connection(("127.0.0.1", 5002), timeout=t); s.settimeout(t)
    s.sendall((c + "\n").encode())
    d = b""
    while b"\n" not in d:
        ch = s.recv(4096)
        if not ch: break
        d += ch
    s.close()
    return d.decode(errors="replace").strip()

def state():
    out = {}
    for k, v in re.findall(r'(\w+)=(\S+)', verb("state")):
        if v.startswith("0x"): continue
        try: out[k] = int(v)
        except ValueError: pass
    return out

# Reach the live city exactly as the canonical harness does.
B = 0x8000
for _ in range(90):
    verb("tap %04X 6" % B, t=8)
    time.sleep(0.30)

print("info:", verb("info"))

ARMS = [
    ("A rskip=0  anlz=1", "rskip 0",  "anlz 1", 0),
    ("B rskip=0  anlz=0", "rskip 0",  "anlz 0", 0),
    ("C rskip=15 anlz=0", "rskip 15", "anlz 0", 0),
    ("D rskip=2  anlz=0", "rskip 2",  "anlz 0", 0),
    ("E mode=1   anlz=0", "rskip 0",  "anlz 0", 1),
]

samples = {name: [] for name, _, _, _ in ARMS}
ROUNDS, WINDOW = 3, 15.0

def set_arm(name, rskip, anlz, mode):
    verb(rskip); verb(anlz); verb("mode %d" % mode)
    verb("cclog 0")
    time.sleep(2.0)

for r in range(ROUNDS):
    order = ARMS if r % 2 == 0 else list(reversed(ARMS))
    for name, rskip, anlz, mode in order:
        set_arm(name, rskip, anlz, mode)
        a = state(); t0 = time.time()
        time.sleep(WINDOW)
        b = state(); t1 = time.time()
        dt = t1 - t0
        df = max(b["frame"] - a["frame"], 1)
        di = b["instructions"] - a["instructions"]
        rec = dict(
            fps=df / dt,
            core_s=b["core"] / 1e6,
            loop_s=b["loop"] / 1e6,
            instr_per_frame=di / df,
            render_ms=b["render"] / 1e3,
            present_ms=b["present"] / 1e3,
            dbg_ms=b["dbg"] / 1e3,
        )
        samples[name].append(rec)
        print("  r%d %-20s %6.2f fps  core %6.3f s  instr/f %7.0f  render %6.1f ms" %
              (r, name, rec["fps"], rec["core_s"], rec["instr_per_frame"], rec["render_ms"]))

print("\n=== medians over %d rounds ===" % ROUNDS)
print("%-20s %8s %9s %11s %9s %8s" % ("arm", "fps", "core_s", "instr/f", "render_ms", "ms/frame"))
med = {}
for name, _, _, _ in ARMS:
    ss = samples[name]
    m = {k: statistics.median(s[k] for s in ss) for k in ss[0]}
    med[name] = m
    print("%-20s %8.2f %9.3f %11.0f %9.1f %8.1f" %
          (name, m["fps"], m["core_s"], m["instr_per_frame"], m["render_ms"], 1000.0 / m["fps"]))

def fit(xs, ys):
    n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
    d = sum((x - mx) ** 2 for x in xs)
    a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / d
    b = my - a * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (a * x + b)) ** 2 for x, y in zip(xs, ys))
    r2 = 1 - ss_res / ss_tot if ss_tot else 0.0
    return a, b, r2

names = [n for n, _, _, _ in ARMS]
ys = [1000.0 / med[n]["fps"] for n in names]
print("\n=== regression: host ms/frame = a*work + overhead ===")
for label, key, scale in (("emulated cycles (core s/frame)", "core_s", 1.0),
                          ("executed instructions/frame", "instr_per_frame", 1.0)):
    xs = [med[n][key] / scale for n in names]
    if max(xs) == min(xs):
        print("  %-32s : no spread, cannot fit" % label); continue
    a, b, r2 = fit(xs, ys)
    print("  %-32s : ms/frame = %.4f * %s + %.2f   (R^2 %.3f)"
          % (label, a, "x", b, r2))
    if b > 0:
        print("      overhead-only ceiling: %.1f fps" % (1000.0 / b))
PY
pkill -f "simcity-headless" 2>/dev/null || true
echo "--- emulator own summary ---"
grep -E "Ran .* frames|error|Error" /tmp/decompose.out | tail -5 || true
SCRIPT
