#!/usr/bin/env python3
"""
netfb.py — pull live pixels off a running SimCity 3DS over the debug port.

The point of this script is that it lets the HOST look at the 3DS screen. A
verbal description of "it looks repeated and black and white" is not evidence;
a decoded PNG is. Everything it needs comes from two extension verbs on the
debug socket:

    state              -> where the core's frame lives, and whether it is alive
    fbsize <screen>    -> real framebuffer geometry (never hardcoded)
    fbrd <scr> <off> <n> -> framebuffer bytes, hex, one chunk per reply

The core frame itself is read with the existing `dumpmem` verb, using the
address `state` hands out, so no new memory verb was needed.

Usage:
    python3 tools/netfb.py --host 10.0.0.130 --out debug/net
"""

import argparse
import binascii
import re
import socket
import struct
import sys

import zlib

GFX_TOP, GFX_BOTTOM = 0, 1


def write_png(path, width, height, rgb_rows):
    """Minimal PNG encoder: 8-bit truecolour, no interlacing, no filtering."""
    raw = b"".join(b"\x00" + row for row in rgb_rows)

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(png)
    return len(png)


def rgb565_to_rows(buf, width, height):
    rows = []
    for y in range(height):
        row = bytearray()
        base = y * width * 2
        for x in range(width):
            v = struct.unpack_from("<H", buf, base + x * 2)[0]
            r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r5 << 3) | (r5 >> 2),
                          (g6 << 2) | (g6 >> 4),
                          (b5 << 3) | (b5 >> 2)))
        rows.append(bytes(row))
    return rows


def bgra_to_rows(buf, width, height):
    """The core emits uint32_t BGRA; byte order in memory is B, G, R, A."""
    rows = []
    for y in range(height):
        row = bytearray()
        base = y * width * 4
        for x in range(width):
            b, g, r = buf[base + x * 4], buf[base + x * 4 + 1], buf[base + x * 4 + 2]
            row += bytes((r, g, b))
        rows.append(bytes(row))
    return rows


class Bridge:
    def __init__(self, host, port, timeout=20.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.rfile = self.sock.makefile("rwb")

    def cmd(self, line):
        self.rfile.write((line + "\n").encode())
        self.rfile.flush()
        reply = self.rfile.readline()
        if not reply:
            raise IOError("bridge closed the connection")
        return reply.decode("ascii", "replace").rstrip("\n")

    def close(self):
        try:
            self.rfile.close()
            self.sock.close()
        except OSError:
            pass


def _fbrd_chunk(b, screen, off, want, tries=6):
    """Read one chunk, tolerating double buffering but not a moving image.

    Two distinct hazards, handled differently on purpose:

    * Emulation advancing. `memset` at the top of every frame leaves a window in
      which the buffer is genuinely half old and half new. No amount of re-reading
      fixes that, so we give up loudly instead of returning a blend.
    * gfxSwapBuffers() still runs every VBlank while the game is paused, so
      gfxGetFramebuffer() alternates between two buffers. Both hold real
      images. Reading an even number of times and requiring every even-indexed
      read to agree lands consistently in the same buffer.
    """
    reads = []
    for _ in range(tries):
        reply = b.cmd("fbrd %d %d %d" % (screen, off, want))
        if reply.startswith("ERR"):
            raise RuntimeError("fbrd at %d said: %s" % (off, reply))
        got_off, got_len, hexdata = reply.split(" ", 2)
        if int(got_off) != off or int(got_len) != want:
            raise RuntimeError("out-of-order chunk: asked %d/%d, got %s/%s"
                               % (off, want, got_off, got_len))
        reads.append(hexdata.strip())

    even = reads[0::2]
    if len(even) >= 2 and len(set(even)) == 1:
        return binascii.unhexlify(reads[0].strip())

    raise RuntimeError(
        "chunk %d never settled after %d reads (%d distinct values). "
        "The emulation is advancing — the capture would be a blend of frames. "
        "Pause the game first (START on the device, or the `pause` verb)."
        % (off, tries, len(set(reads))))


def pull_framebuffer(b, screen, chunk=1024):
    size = b.cmd("fbsize %d" % screen)
    if not size.startswith("OK"):
        raise RuntimeError("fbsize said: %s" % size)
    _, w, h, total = size.split()
    w, h, total = int(w), int(h), int(total)
    print("  framebuffer: %dx%d, %d bytes" % (w, h, total))

    buf = bytearray()
    offset = 0
    while offset < total:
        want = min(chunk, total - offset)
        buf += _fbrd_chunk(b, screen, offset, want)
        offset += want
        if offset % (32 * chunk) == 0:
            print("    %d/%d" % (offset, total))
    return w, h, bytes(buf)


def pull_core_frame(b, addr, width, height):
    """Uses the pre-existing `dumpmem` verb; replies are 'OK dumping' + raw + '\\n.\\n'."""
    total = width * height * 4
    buf = bytearray()
    chunk = 1024
    offset = 0
    while offset < total:
        want = min(chunk, total - offset)
        addr_here = addr + offset
        b.rfile.write(("dumpmem 0x%x %d\n" % (addr_here, want)).encode())
        b.rfile.flush()
        head = b.rfile.readline().decode("ascii", "replace").strip()
        if head != "OK dumping":
            raise RuntimeError("dumpmem at 0x%x said: %r" % (addr_here, head))
        data = b.rfile.read(want)
        if len(data) != want:
            raise RuntimeError("short read at 0x%x: %d/%d" % (addr_here, len(data), want))
        buf += data
        tail = b.rfile.read(3)  # "\n.\n"
        if tail != b"\n.\n":
            raise RuntimeError("bad terminator at 0x%x: %r" % (addr_here, tail))
        offset += want
        if offset % (32 * chunk) == 0:
            print("    %d/%d" % (offset, total))
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.0.0.130")
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--out", default="debug/net")
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    import os
    os.makedirs(args.out, exist_ok=True)
    tag = ("_" + args.tag) if args.tag else ""

    b = Bridge(args.host, args.port)
    try:
        print("ping:", b.cmd("ping"))
        print("info:", b.cmd("info"))

        state = b.cmd("state")
        print("state:", state)
        if not state.startswith("OK"):
            return 1
        kv = dict(re.findall(r"(\w+)=(\S+)", state[3:]))
        src_addr = int(kv.get("src", "0"), 16)
        sw, sh = int(kv.get("src_w", "0")), int(kv.get("src_h", "0"))
        print("  core frame: addr=0x%08x %dx%d" % (src_addr, sw, sh))

        fw, fh, fbuf = pull_framebuffer(b, GFX_TOP)
        n = write_png("%s/fb_top%s.png" % (args.out, tag), fw, fh,
                      rgb565_to_rows(fbuf, fw, fh))
        print("wrote %s/fb_top%s.png (%d bytes PNG)" % (args.out, tag, n))

        if src_addr and sw and sh:
            cbuf = pull_core_frame(b, src_addr, sw, sh)
            n = write_png("%s/core%s.png" % (args.out, tag), sw, sh,
                          bgra_to_rows(cbuf, sw, sh))
            print("wrote %s/core%s.png (%d bytes PNG)" % (args.out, tag, n))
            with open("%s/core%s.raw" % (args.out, tag), "wb") as fh_:
                fh_.write(cbuf)
        else:
            print("  core frame unavailable (src=0) — skipped")

        with open("%s/fb_top%s.raw" % (args.out, tag), "wb") as fh_:
            fh_.write(fbuf)
        with open("%s/state%s.txt" % (args.out, tag), "w") as fh_:
            fh_.write(state[3:] + "\n")
        return 0
    finally:
        b.close()


if __name__ == "__main__":
    sys.exit(main())