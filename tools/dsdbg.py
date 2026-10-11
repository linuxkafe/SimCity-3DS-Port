#!/usr/bin/env python3
"""Host-side driver for the SimCity 3DS debug channel (curl only).

The 3DSX polls sdmc:/cmd.txt once per frame, runs the command, and writes
sdmc:/resp.txt. The device deletes cmd.txt when it consumes it, which is the
acknowledgement we wait for. Both files live on the SD root and are reachable
over the same anonymous FTP the build was delivered through, so this is a full
request/response loop with no button pressing.

Commands:  ping | info | rot <0|1> | dump fb
"""
import os
import subprocess
import sys
import tempfile
import time

USER = "anonymous:."


def url(host, port, path):
    return f"ftp://{USER}@{host}:{port}/{path}"


def _curl(args, timeout):
    return subprocess.run(["curl", "-sS", *args], capture_output=True,
                          timeout=timeout)


def send_cmd(host, port, cmd, timeout=20, retries=6):
    """Upload cmd.txt, wait for the device to consume it, read resp.txt."""
    payload = (cmd + "\n").encode()
    last_err = ""
    uploaded = False
    for _ in range(retries):
        fd, tmp = tempfile.mkstemp(suffix=".cmd")
        try:
            os.write(fd, payload)
            os.close(fd)
            r = _curl(["-T", tmp, url(host, port, "cmd.txt")], 40)
            if r.returncode == 0:
                uploaded = True
                break
            last_err = r.stderr.decode(errors="replace")
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
        time.sleep(1.0)
    if not uploaded:
        raise SystemExit(f"cmd.txt upload failed: {last_err}")

    # The device removes cmd.txt once it has read it.
    deadline = time.time() + timeout
    while time.time() < deadline:
        fd, probe = tempfile.mkstemp(suffix=".probe")
        os.close(fd)
        r = _curl(["-o", probe, url(host, port, "cmd.txt")], 20)
        size = os.path.getsize(probe) if os.path.exists(probe) else -1
        os.unlink(probe)
        if r.returncode != 0 or size <= 0:   # gone -> consumed
            break
        time.sleep(0.4)

    fd, out = tempfile.mkstemp(suffix=".resp")
    os.close(fd)
    _curl(["-o", out, url(host, port, "resp.txt")], 20)
    text = open(out).read() if os.path.exists(out) else ""
    os.unlink(out)
    return text.strip()


def fetch(host, port, remote, local, retries=5):
    for _ in range(retries):
        r = _curl(["-o", local, url(host, port, remote)], 90)
        if r.returncode == 0 and os.path.exists(local) and os.path.getsize(local) > 0:
            return os.path.getsize(local)
        time.sleep(1.0)
    raise SystemExit(f"fetch {remote} failed")


if __name__ == "__main__":
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    host, port, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
    arg = sys.argv[4] if len(sys.argv) > 4 else ""
    print(send_cmd(host, port, f"{cmd} {arg}".strip()))