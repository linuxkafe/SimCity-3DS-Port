#!/usr/bin/env python3
"""Convert a 3DSX diagnostic dump (.raw) into a PNG the agent can actually look at.

Header layout (written by frontend/3ds/3ds_dbg.c):
    char magic[4] = "SDBG"; u32 version; u32 width; u32 height;
    u32 stride; u32 bpp;   -> 24 bytes
Payload follows immediately: bpp/8 bytes per pixel, row-major.

RGB565 and BGRA32 payloads are both supported. No third-party deps.
"""
import struct, sys, zlib

HDR = struct.Struct("<4s5I")

def read_raw(path):
    d = open(path, "rb").read()
    if len(d) < HDR.size:
        raise SystemExit(f"{path}: too small ({len(d)} bytes) for header")
    magic, ver, w, h, stride, bpp = HDR.unpack_from(d, 0)
    if magic != b"SDBG":
        raise SystemExit(f"{path}: bad magic {magic!r}, expected b'SDBG'")
    px = d[HDR.size:]
    print(f"{path}: {w}x{h} stride={stride} bpp={bpp} version={ver} payload={len(px)}B")
    need = stride * h
    if len(px) < need:
        raise SystemExit(f"{path}: truncated payload, need {need}B have {len(px)}B")
    return w, h, stride, bpp, px

def to_rgb(w, h, stride, bpp, px):
    """Return a bytearray of w*h*3 RGB."""
    out = bytearray(w * h * 3)
    if bpp == 16:
        for y in range(h):
            row = y * stride
            base = y * w * 3
            for x in range(w):
                v = struct.unpack_from("<H", px, row + x * 2)[0]
                r = (v >> 11) & 0x1F
                g = (v >> 5) & 0x3F
                b = v & 0x1F
                o = base + x * 3
                out[o]     = (r * 255) // 31      # 5-bit
                out[o + 1] = (g * 255) // 63      # 6-bit
                out[o + 2] = (b * 255) // 31
    elif bpp == 32:
        # core emits BGRA little-endian: byte0=B, byte1=G, byte2=R
        for y in range(h):
            row = y * stride
            base = y * w * 3
            for x in range(w):
                o0 = row + x * 4
                b, g, r = px[o0], px[o0 + 1], px[o0 + 2]
                o = base + x * 3
                out[o] = r
                out[o + 1] = g
                out[o + 2] = b
    else:
        raise SystemExit(f"unsupported bpp {bpp}")
    return out

def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(rgb[y*w*3:(y+1)*w*3]) for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        raise SystemExit("usage: raw2png.py <in.raw> <out.png>")
    w, h, stride, bpp, px = read_raw(sys.argv[1])
    write_png(sys.argv[2], w, h, to_rgb(w, h, stride, bpp, px))
    print(f"wrote {sys.argv[2]} ({w}x{h})")
