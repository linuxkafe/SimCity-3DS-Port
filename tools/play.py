#!/usr/bin/env python3
"""
play.py — drive the running 3DS SimCity over the debug port and capture frames.

Two things this exists to get right, both learned the hard way:

  * `step N` decrements once per main-loop iteration, and the loop runs at
    roughly 2.5 fps, so `step 300` takes about two minutes. Issuing a command
    and immediately capturing reads the screen mid-queue. Always drain first.

  * The title screen animates — about 13% of pixels change over 40 frames with
    no input at all. So "the framebuffer hash changed" is NOT evidence that a
    button did anything. Screenshots are compared by eye; hashes are only ever
    used to detect that a screen is static enough to capture reliably.

Usage:
    python3 tools/play.py --input 0x1000 --step 10 --tag menu
    python3 tools/play.py --release --tag idle
"""
import argparse, os, socket, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SNES = {"B":0x8000,"Y":0x4000,"SELECT":0x2000,"START":0x1000,"UP":0x0800,
        "DOWN":0x0400,"LEFT":0x0200,"RIGHT":0x0100,"A":0x0080,"X":0x0040,
        "L":0x0020,"R":0x0010}


class Bridge:
    def __init__(self, host, port, timeout=120.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.f = self.s.makefile("rwb")
        # netfb's readers use .rfile; keep both names pointing at one stream
        # rather than maintaining two subtly different clients.
        self.rfile = self.f

    def cmd(self, c):
        self.f.write((c + "\n").encode()); self.f.flush()
        r = self.f.readline()
        if not r:
            raise IOError("bridge closed")
        return r.decode().rstrip("\n")

    def state(self):
        kv = dict(t.split("=", 1) for t in self.cmd("state")[3:].split() if "=" in t)
        return kv

    def frame(self):
        return int(self.state()["frame"])

    def drain(self, limit=600.0):
        """Wait until the frame counter stops moving. Returns the frame."""
        t0 = time.time(); last = self.frame(); stable = 0
        while time.time() - t0 < limit:
            cur = self.frame()
            if cur == last:
                stable += 1
                if stable >= 3:
                    return cur
            else:
                stable = 0
            last = cur
            time.sleep(0.6)
        raise TimeoutError("emulation never settled")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.0.0.130")
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--input", help="SNES mask, hex (0x1000) or names (START+A)")
    ap.add_argument("--release", action="store_true")
    ap.add_argument("--step", type=int, default=0)
    ap.add_argument("--tag", default="play")
    ap.add_argument("--out", default="debug/net")
    args = ap.parse_args()

    mask = 0
    if args.input:
        for part in args.input.replace("+", " ").split():
            if part.upper() in SNES:
                mask |= SNES[part.upper()]
            else:
                mask |= int(part, 0)

    b = Bridge(args.host, args.port)
    try:
        print("pause ->", b.cmd("pause")); time.sleep(0.5)
        print("input ->", b.cmd("input 0x%04X" % mask) if not args.release
                         else b.cmd("input 0"))
        if args.step:
            print("step  ->", b.cmd("step %d" % args.step))
        fr = b.drain()
        print("settled at frame", fr)

        import netfb
        b.cmd("pause"); time.sleep(0.6)
        fw, fh, fbuf = netfb.pull_framebuffer(b, netfb.GFX_TOP)
        st = b.state()
        aw, ah = int(st["src_w"]), int(st["src_h"])
        addr = int(st["src"], 16)

        cbuf = netfb.pull_core_frame(b, addr, aw, ah)
        os.makedirs(args.out, exist_ok=True)
        n1 = netfb.write_png("%s/fb_%s.png" % (args.out, args.tag), fw, fh,
                             netfb.rgb565_to_rows(fbuf, fw, fh))
        n2 = netfb.write_png("%s/core_%s.png" % (args.out, args.tag), aw, ah,
                             netfb.bgra_to_rows(cbuf, aw, ah))
        print("wrote %s/fb_%s.png (%d) and core_%s.png (%d)"
              % (args.out, args.tag, n1, args.tag, n2))
        print("state:", " ".join("%s=%s" % kv for kv in st.items()))
    finally:
        b.s.close()


if __name__ == "__main__":
    sys.exit(main())
