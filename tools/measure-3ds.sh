#!/usr/bin/env bash
# Measure the port on the 3DS against the metric that decides the frame rate.
#
# Native x86 time is NOT the metric that matters. The emulator charges the
# guest ARM11 cycles, and the port's phase timings come from the guest's own
# tick counter, so the number that decides both the emulator's frames per
# second and the device's emulated performance is the guest's emulated clock
# per frame -- the `core` phase that `state` reports.
#
# Optimising against native x86 time has already misled once: the render
# hoisting in upstream T059 was 1.43x natively and 2 % on the 3DS.
#
# Usage: measure-3ds.sh [rom-on-deck]
#
# Rebuilds the 3DSX from the current upstream, deploys it, runs it under the
# headless Azahar build, drives it into the live city with the T048 cadence,
# and reports the emulated clock per frame and the host-visible frame rate.
set -euo pipefail

PORT_DIR="${PORT_DIR:-/home/seyon/dev/Games/Nintendo/3DS/SimCity3DS}"
UPSTREAM="${UPSTREAM:-/home/seyon/dev/Games/PC/SimCity-SNES-Static-Recomp}"
DECK="${DECK:-deck@steamdeck}"
ROM="${1:-/home/deck/simcity-test/simcity_measure.3dsx}"
NAME="$(basename "$ROM" .3dsx)"

echo "== rebuilding the 3DSX from $UPSTREAM =="
cd "$PORT_DIR"
make docker-build >/tmp/measure-3ds-build.log 2>&1 || {
    echo "build failed:"; tail -20 /tmp/measure-3ds-build.log; exit 1; }
scp -q build/simcity.3dsx "$DECK:$ROM"
echo "   deployed $ROM"

echo "== measuring =="
ssh -o BatchMode=yes "$DECK" bash -s -- "$ROM" <<'SCRIPT'
set -u
ROM="$1"
SDMC=/home/deck/.local/share/azahar-emu/sdmc
cd /home/deck/simcity-test
pkill -f "simcity-headless" 2>/dev/null || true
sleep 2
rm -f "$SDMC/simcity-trace.log"
timeout 300 ./simcity-headless --max-seconds 240 "$ROM" >/dev/null 2>&1 &
sleep 25
python3 - <<'PY'
import socket, time, re
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
# Drive to the live city with the T048 cadence so every build is measured in
# the same game state. Without this the phase numbers drift by up to 42 %.
B = 0x8000
for _ in range(90):
    verb("tap %04X 6" % B, t=8)
    time.sleep(0.30)
a = state(); t0 = time.time(); time.sleep(60); b = state(); t1 = time.time()
dt = t1 - t0; df = max(b["frame"] - a["frame"], 1)
print("  emulated clock per frame : core %.3f s   loop %.3f s" %
      (b["core"] / 1e6, b["loop"] / 1e6))
print("  host-visible frame rate  : %.3f fps" % (df / dt))
print("  guest instr/frame        : %d" % ((b["instructions"] - a["instructions"]) / df))
print("  other phases             : render %.1f ms  dbg %.1f ms  present %.1f ms" %
      (b["render"] / 1e3, b["dbg"] / 1e3, b["present"] / 1e3))
PY
pkill -f "simcity-headless" 2>/dev/null || true
SCRIPT
