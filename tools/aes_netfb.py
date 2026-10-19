#!/usr/bin/env python3
"""
AES-compliant netfb — captures frames via fresh connection per command.
Used for T022 playtest verification.
"""
import argparse, binascii, os, re, socket, struct, sys, time, zlib

GFX_TOP, GFX_BOTTOM = 0, 1

def send_cmd(cmd, host, port, timeout=10):
    """Send command with fresh connection, return stripped reply."""
    s = socket.socket()
    s.settimeout(timeout)
    s.connect((host, port))
    s.send((cmd + "\n").encode())
    data = b""
    start = time.time()
    while time.time() - start < timeout:
        try:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
            if b"\n" in data:
                if cmd.startswith("fbrd"):
                    parts = data.split(b" ", 2)
                    if len(parts) == 3 and parts[2].endswith(b"\n"):
                        break
                elif cmd.startswith("dumpmem"):
                    if data.endswith(b"\n.\n"): break
                else:
                    break
        except socket.timeout: break
    s.close()
    return data.decode("ascii", "replace").strip()

def send_cmd_bytes(cmd_bytes, host, port, timeout=10):
    """For binary dumps (dumpmem)."""
    s = socket.socket()
    s.settimeout(timeout)
    s.connect((host, port))
    s.send(cmd_bytes)
    data = b""
    start = time.time()
    while time.time() - start < timeout:
        try:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
            if data.endswith(b"\n.\n"): break
        except socket.timeout: break
    s.close()
    return data

def rgb565_to_png(buf, width, height, path):
    rows = []
    for y in range(height):
        row = bytearray()
        base = y * width * 2
        for x in range(width):
            v = struct.unpack_from("<H", buf, base + x * 2)[0]
            r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)))
        rows.append(bytes(row))
    raw = b"".join(b"\x00" + row for row in rows)
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f: f.write(png)
    return len(png)

def bgra_to_png(buf, width, height, path):
    rows = []
    for y in range(height):
        row = bytearray()
        base = y * width * 4
        for x in range(width):
            b, g, r = buf[base + x * 4], buf[base + x * 4 + 1], buf[base + x * 4 + 2]
            row += bytes((r, g, b))
        rows.append(bytes(row))
    raw = b"".join(b"\x00" + row for row in rows)
    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f: f.write(png)
    return len(png)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.0.0.128")
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--out", default="aes/verification/T022")
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    tag = ("_" + args.tag) if args.tag else ""

    print(f"[{time.strftime('%H:%M:%S')}] Connecting to {args.host}:{args.port}")

    # Ping + info
    print("ping:", send_cmd("ping", args.host, args.port))
    info = send_cmd("info", args.host, args.port)
    print("info:", info)

    # State
    state = send_cmd("state", args.host, args.port)
    print("state:", state)
    if not state.startswith("OK"):
        return 1
    kv = dict(re.findall(r"(\w+)=(\S+)", state[3:]))
    src_addr = int(kv.get("src", "0"), 16)
    sw, sh = int(kv.get("src_w", "0")), int(kv.get("src_h", "0"))
    frame = kv.get("frame", "?")
    print(f"  core frame: addr=0x{src_addr:08x} {sw}x{sh} frame={frame}")

    # Top framebuffer
    print(f"[{time.strftime('%H:%M:%S')}] Capturing top framebuffer (240x400)...")
    total = 240 * 400 * 2
    buf = bytearray()
    for off in range(0, total, 1024):
        want = min(1024, total - off)
        reply = send_cmd(f"fbrd 0 {off} {want}", args.host, args.port)
        if reply.startswith("ERR"):
            print("ERROR fbrd:", reply); return 1
        _, _, hexdata = reply.split(" ", 2)
        buf += binascii.unhexlify(hexdata.strip())
        if off % 32768 == 0:
            print(f"  {off}/{total}")

    n = rgb565_to_png(bytes(buf), 240, 400, f"{args.out}/fb_top{tag}.png")
    print(f"wrote {args.out}/fb_top{tag}.png ({n} bytes PNG, {len(buf)} raw)")
    with open(f"{args.out}/fb_top{tag}.raw", "wb") as f: f.write(buf)

    # Core frame
    if src_addr and sw and sh:
        print(f"[{time.strftime('%H:%M:%S')}] Capturing core frame ({sw}x{sh})...")
        total = sw * sh * 4
        cbuf = bytearray()
        chunk_size = 1024
        for off in range(0, total, chunk_size):
            want = min(chunk_size, total - off)
            addr = src_addr + off
            data = send_cmd_bytes(f"dumpmem 0x{addr:x} {want}\n".encode(), args.host, args.port)
            if not data.startswith(b"OK dumping"):
                print("ERROR dumpmem:", data[:100]); return 1
            head_end = data.find(b"\n") + 1
            cbuf += data[head_end:-3]
            if off % 32768 == 0:
                print(f"  {off}/{total}")

        n = bgra_to_png(bytes(cbuf), sw, sh, f"{args.out}/core{tag}.png")
        print(f"wrote {args.out}/core{tag}.png ({n} bytes PNG, {len(cbuf)} raw)")
        with open(f"{args.out}/core{tag}.raw", "wb") as f: f.write(cbuf)
    else:
        print("  core frame unavailable (src=0) — skipped")

    with open(f"{args.out}/state{tag}.txt", "w") as f: f.write(state[3:] + "\n")
    print(f"[{time.strftime('%H:%M:%S')}] DONE")
    return 0

if __name__ == "__main__":
    sys.exit(main())
