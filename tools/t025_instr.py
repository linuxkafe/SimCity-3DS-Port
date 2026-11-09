#!/usr/bin/env python3
"""Read the T025 instrumentation off the Deck: state verb + instr_frame."""
import socket
import subprocess
import sys
import time

HOST = "deck@steamdeck"


def ssh(cmd, timeout=60):
    r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                        HOST, cmd], capture_output=True, text=True,
                       timeout=timeout)
    return r.stdout, r.stderr


def dbg(cmd, timeout=8):
    """One debug-port command, fresh connection each time (the bridge tears
    the connection down after each command)."""
    script = r'''
import socket, time, sys
cmd = sys.argv[1]
s = socket.socket(); s.settimeout(5)
try:
    s.connect(("10.0.0.128", 5002))
except Exception as e:
    print("CONNECT-FAIL", e); sys.exit(0)
s.send(cmd.encode() + b"\n")
data = b""
end = time.time() + 4
while time.time() < end:
    try:
        ch = s.recv(65536)
        if not ch: break
        data += ch
        if data.endswith(b"\n") and len(data) > 4: break
    except Exception:
        break
s.close()
print(data.decode(errors="replace").strip())
'''
    p = subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, "python3 -c",
                        script, cmd], capture_output=True, text=True,
                       timeout=timeout)
    return p.stdout.strip()


def main():
    out, err = ssh("pgrep -a azahar | head -2")
    print("process:", out.strip() or "(none)")

    st = dbg("state")
    print("\nstate:")
    print(st)

    if "instr_frame=" not in st:
        print("\n(instr_frame absent from state -- debug port not reachable)")
        return 1

    vals = []
    for field in st.split():
        if field.startswith("instr_frame="):
            vals.append(int(field.split("=", 1)[1]))
    print("\ninstr_frame samples:", vals)
    if vals:
        vals_sorted = sorted(vals)
        print("min=%d max=%d median=%d spread=%.1f%%" % (
            vals_sorted[0], vals_sorted[-1],
            vals_sorted[len(vals) // 2],
            100.0 * (vals_sorted[-1] - vals_sorted[0]) /
            max(1, vals_sorted[len(vals) // 2])))
        print("stable (within 10%%):",
              (max(vals) - min(vals)) <= 0.10 * max(1, vals_sorted[len(vals)//2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
