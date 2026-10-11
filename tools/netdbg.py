#!/usr/bin/env python3
"""Host-side TCP client for the SimCity 3DS debug server.

The 3DSX runs a non-blocking TCP server on port 5000 (see
frontend/3ds/3ds_dbg.c). This speaks to it directly: send a one-line command,
read the reply. For 'dump'/'dumpsrc' the reply is followed by a binary blob
terminated by "\\n.\\n"; the blob is written to a .raw file next to the PNG
converter (tools/raw2png.py).

Commands:  ping | info | rot <0|1> | dump | dumpsrc
"""
import socket
import sys
import time

HOST_DEFAULT = "10.0.0.115"
PORT_DEFAULT = 5002
TIMEOUT = 20


def connect(host, port, timeout=TIMEOUT):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    # The server does not greet; it waits for the first command.
    return s, b""


def send_cmd(s, cmd, timeout=TIMEOUT):
    s.sendall((cmd + "\n").encode())
    s.settimeout(timeout)
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.decode(errors="replace").strip()


def recv_blob(s, timeout=TIMEOUT):
    """Read until the '\\n.\\n' sentinel. Returns raw bytes."""
    s.settimeout(timeout)
    data = b""
    while not data.endswith(b"\n.\n"):
        chunk = s.recv(16384)
        if not chunk:
            break
        data += chunk
    if data.endswith(b"\n.\n"):
        data = data[:-3]
    return data


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else HOST_DEFAULT
    port = int(sys.argv[2]) if len(sys.argv) > 2 else PORT_DEFAULT
    cmd = sys.argv[3] if len(sys.argv) > 3 else "ping"
    arg = sys.argv[4] if len(sys.argv) > 4 else ""
    outfile = sys.argv[5] if len(sys.argv) > 5 else None

    s, greet = connect(host, port)
    if greet:
        print(f"[greet] {greet.decode(errors='replace').strip()}")
    reply = send_cmd(s, f"{cmd} {arg}".strip())
    print(f"[reply] {reply}")

    if cmd in ("dump", "dumpsrc"):
        blob = recv_blob(s)
        name = outfile or (f"debug/net_{cmd}.raw")
        with open(name, "wb") as f:
            f.write(blob)
        print(f"[blob] {len(blob)} bytes -> {name}")
    s.close()


if __name__ == "__main__":
    main()