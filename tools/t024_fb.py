"""T024 framebuffer capture: grab the top screen and compare pixel format.

The `fbrd` verb dumps a screen in chunks: `fbrd <screen> <offset> <length>`
- screen: 0=top, 1=bottom
- offset: byte offset into framebuffer
- length: bytes to read (max 1024 = MAX_RSP_MEM)

The top framebuffer is 240x400x2 = 192,000 bytes (BGR565).
This script loops through all chunks and assembles the full frame.

The colour question (T024): Azahar logs
  "Format B8G8R8Unorm unsupported, falling back unconditionally to R8G8B8A8Unorm"
This script captures the raw framebuffer so we can check whether the pixels
are BGR565 (what the 3DS GPU expects) or RGBA8888 (what the fallback produces).

The debug bridge tears the connection after each command (see aes_prof.py).
We must open a fresh connection per chunk.
"""
import argparse, socket, sys, time, struct

def recv_one(s, timeout=5):
    """Read one '\n'-terminated reply."""
    data = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if data.endswith(b"\n") and len(data) > 4:
                break
        except socket.timeout:
            break
    return data

def read_chunk(host, port, screen, offset, length):
    """Read one chunk via fbrd using a fresh connection. Returns (ok, bytes or error_msg)."""
    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect((host, port))
        cmd = f"fbrd {screen} {offset} {length}\n".encode()
        s.send(cmd)
        reply = recv_one(s)
        reply = reply.strip()
        if reply == b".":
            return False, b"end"
        if not reply:
            return False, b"empty"
        # Reply format: "<offset> <length> <hex>"
        parts = reply.split(b" ", 2)
        if len(parts) < 3 or not parts[0].isdigit():
            return False, reply
        try:
            chunk_data = bytes.fromhex(parts[2].decode())
            return True, chunk_data
        except ValueError:
            return False, reply
    finally:
        s.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=5002)
    ap.add_argument("--out", default="/tmp/fb_top.raw")
    ap.add_argument("--screen", type=int, default=0, help="0=top, 1=bottom")
    ap.add_argument("--chunk-size", type=int, default=1024)
    args = ap.parse_args()

    # First, query the screen size via info verb
    s = socket.socket()
    s.settimeout(5)
    s.connect((args.host, args.port))
    s.send(b"info\n")
    info = recv_one(s)
    s.close()
    print(f"info: {info[:120].decode(errors='replace')}")

    # Top screen is 240x400, BGR565 = 192000 bytes
    # Bottom is 240x320, BGR565 = 153600 bytes
    total_bytes = 192000 if args.screen == 0 else 153600
    print(f"capturing screen {args.screen} ({total_bytes} bytes) in {args.chunk_size}-byte chunks")

    raw = bytearray(total_bytes)
    chunks = 0
    for offset in range(0, total_bytes, args.chunk_size):
        length = min(args.chunk_size, total_bytes - offset)
        ok, data = read_chunk(args.host, args.port, args.screen, offset, length)
        if not ok:
            print(f"chunk {chunks} failed at offset {offset}: {data[:80]!r}")
            break
        if len(data) != length:
            print(f"chunk {chunks}: expected {length} bytes, got {len(data)}")
        raw[offset:offset+len(data)] = data
        chunks += 1
        if chunks % 32 == 0:
            print(f"  {chunks} chunks, {offset+len(data)} bytes...", flush=True)

    print(f"\ntotal: {chunks} chunks, {len(raw)} bytes (expected {total_bytes})")

    with open(args.out, "wb") as f:
        f.write(raw)
    print(f"written to {args.out}")

    # Quick format analysis
    if len(raw) >= 8:
        p16 = struct.unpack("<H", raw[0:2])[0]
        p32 = struct.unpack("<I", raw[0:4])[0]
        print(f"first 2 bytes as u16le: 0x{p16:04x}")
        print(f"first 4 bytes as u32le: 0x{p32:08x}")
        r565 = p16 & 0x1F
        g565 = (p16 >> 5) & 0x3F
        b565 = (p16 >> 11) & 0x1F
        print(f"  as BGR565: R={r565:2d} G={g565:2d} B={b565:2d}")
        r888 = p32 & 0xFF
        g888 = (p32 >> 8) & 0xFF
        b888 = (p32 >> 16) & 0xFF
        a888 = (p32 >> 24) & 0xFF
        print(f"  as RGBA8888: R={r888:3d} G={g888:3d} B={b888:3d} A={a888:3d}")

    return 0

if __name__ == "__main__":
    sys.exit(main())