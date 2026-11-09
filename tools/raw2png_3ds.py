#!/usr/bin/env python3
"""Convert a 3DS framebuffer dump written by the debug bridge into a PNG.

The bridge's `fbdump` verb writes, in the guest's filesystem:

    u16 width
    u16 height
    width * height * 2 bytes of RGB565

That is the layout `dbgk_dump_screen()` in frontend/3ds/3dsdbg.c writes. It
is not the "SDBG" header that tools/raw2png.py expects -- that one comes from
the host-test build -- so this converter is the one for dumps taken on the
device.

Usage: raw2png_3ds.py <input.raw> [output.png]
"""
import struct
import sys
import zlib


def rgb565_to_rgb(rows):
    out = bytearray()
    for row in rows:
        for (px,) in struct.iter_unpack("<H", row):
            r = (px >> 11) & 0x1F
            g = (px >> 5) & 0x3F
            b = px & 0x1F
            out += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    return bytes(out)


def write_png(path, width, height, rgb):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw += b"\x00" + rgb[y * stride:(y + 1) * stride]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".png"
    data = open(src, "rb").read()
    if len(data) < 4:
        raise SystemExit(f"{src}: too small")
    width, height = struct.unpack_from("<HH", data, 0)
    need = width * height * 2
    if len(data) < 4 + need:
        raise SystemExit(f"{src}: truncated, want {4 + need}B, have {len(data)}B")
    print(f"{src}: {width}x{height} RGB565")
    rows = [data[4 + y * width * 2: 4 + (y + 1) * width * 2] for y in range(height)]
    rgb = rgb565_to_rgb(rows)
    write_png(dst, width, height, rgb)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
