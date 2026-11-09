#!/usr/bin/env python3
"""Measure the port's phases on the 3DS through the debug bridge.

The port's phase values are the guest's own tick clock converted to
microseconds by `ds3_ticks_to_us()`. Two traps, both hit while building this:

  * `state` reports every numeric field in DECIMAL. The `src=0x...` pointer is
    the only hex field.
  * The `prof` verb's `now[...]` block is the last frame; the `avg[...]` block
    is an 8-frame exponential moving average. The `core` field of `state` is
    the last frame too.

Usage: measure-phases.py [host] [port] [seconds]
"""
import re
import socket
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.1.0"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5002
SECONDS = int(sys.argv[3]) if len(sys.argv) > 3 else 45

BUTTON_B = 0x8000  # SIMCITY_INPUT_B


def verb(cmd, timeout=30):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(timeout)
    s.sendall((cmd + "\n").encode())
    data = b""
    while b"\n" not in data:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    return data.decode(errors="replace").strip()


def state():
    out = {}
    for key, val in re.findall(r"(\w+)=(\S+)", verb("state")):
        if val.startswith("0x"):
            continue
        try:
            out[key] = int(val)
        except ValueError:
            pass
    return out


def drive_to_city(presses=90):
    """The T048 cadence: press B every 20 frames, held for 6.

    Without this the phase numbers drift by up to 42 % with scene content
    (T023's own note), and two builds are not comparable.
    """
    for _ in range(presses):
        verb("tap %04X 6" % BUTTON_B, timeout=15)
        time.sleep(0.30)


def main():
    drive_to_city()
    a = state()
    t0 = time.time()
    time.sleep(SECONDS)
    b = state()
    dt = time.time() - t0
    df = max(b["frame"] - a["frame"], 1)
    di = b["instructions"] - a["instructions"]

    print("  guest frames      : %d in %.1f s" % (df, dt))
    print("  host-visible fps  : %.2f" % (df / dt))
    print("  guest instr/frame : %d" % (di // df))
    print("  emulated clock/frame:")
    print("    core     %8.3f s  (%5.1f %% of loop)" %
          (b["core"] / 1e6, 100.0 * b["core"] / b["loop"]))
    print("    render   %8.3f ms" % (b["render"] / 1e3))
    print("    dbg      %8.3f ms" % (b["dbg"] / 1e3))
    print("    present  %8.3f ms" % (b["present"] / 1e3))
    print("    loop     %8.3f s" % (b["loop"] / 1e6))
    print("    => guest clock runs %.1fx faster than wall" %
          ((b["loop"] / 1e6) * df / dt))
    print("    => 3DS hardware would run %.3f fps" % (1.0 / (b["loop"] / 1e6)))


if __name__ == "__main__":
    main()
