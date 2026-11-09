#!/usr/bin/env python3
"""bridge.py — Persistent TCP client for the 3DS debug bridge with wedge detection.

Extracted from harness.py and netfb.py. The debug bridge on 3DS (3dsdbg.c) is a
single-client non-blocking TCP server on port 5002. It wedges when a client
disconnects uncleanly (no POLLHUP reap). This module provides:

- Persistent connection with transparent reconnect
- Wedge detection via `pollstat` verb (accepts/hasclient)
- Command verification via wait_for_flag (from harness.py)
- Frame synchronization via step/wait_for_frame
"""

import socket
import time
import re
from typing import Optional, Dict, Any, List


class BridgeError(Exception):
    """Base exception for bridge errors."""
    pass


class BridgeWedgeError(BridgeError):
    """Raised when bridge wedge is detected and cannot auto-recover."""
    pass


class BridgeTimeoutError(BridgeError):
    """Raised when a command times out."""
    pass


class Bridge:
    """One connection to the debug bridge, held open across commands.

    The bridge serves ONE client at a time and has no idle timeout, so a
    single socket can carry the whole run. This exists because opening a
    fresh connection per command (~250 sockets per gate) provoked the
    emulator SIGKILL symptom documented in T034.

    The socket is re-established transparently on any send/receive failure.
    A dropped connection is an event the report shows, not a silent skip.

    Wedge detection: the bridge wedges when a client disappears without a
    clean close. The symptom is the port still LISTENs but commands never
    return. We detect this via `pollstat` — if `hasclient=1` but no reply
    arrives within timeout, the bridge is wedged.
    """

    VERBS = frozenset({
        "ping", "info", "state", "rot", "pause", "run", "step",
        "fbsize", "fbrd", "fbdump", "input", "tap", "prof", "pollstat",
        "cclog", "anlz", "trace"
    })

    def __init__(
        self,
        host: str,
        port: int = 5002,
        timeout: float = 40.0,
        settle: float = 0.45,
        wedge_check_interval: float = 5.0,
    ):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.settle = settle
        self.wedge_check_interval = wedge_check_interval

        self._sock: Optional[socket.socket] = None
        self._fh = None
        self._last_wedge_check = 0.0
        self.reconnects = 0
        self.commands_sent = 0
        self.wedges_detected = 0

    # ─── Connection Management ───

    def _connect(self) -> None:
        self.close()
        self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self._sock.settimeout(self.timeout)
        self._fh = self._sock.makefile("rwb")

    def close(self) -> None:
        if self._fh is not None:
            try:
                self._fh.close()
            except OSError:
                pass
            self._fh = None
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        # Ensure None assignment for type checker
        self._fh = None
        self._sock = None

    def _ensure_connected(self) -> None:
        if self._fh is None:
            self._connect()
            self.reconnects += 1
        # _fh and _sock are guaranteed non-None after _connect()

    # ─── Low-level Command ───

    def _raw_cmd(self, line: str, retries: int = 3) -> str:
        """Send a command on the held connection and read one line.

        Transparently reconnects on failure. The settle delay exists because
        the bridge is serviced from the emulated main loop and a command
        issued mid-frame is answered a frame later.
        """
        last_exc = None
        for attempt in range(retries):
            try:
                self._ensure_connected()
                time.sleep(self.settle)
                assert self._fh is not None
                self._fh.write((line + "\n").encode())
                self._fh.flush()
                reply = self._fh.readline()
                if not reply:
                    raise OSError("bridge closed the connection")
                self.commands_sent += 1
                return reply.decode("ascii", "replace").rstrip("\n")
            except (socket.timeout, ConnectionError, OSError, BrokenPipeError) as exc:
                last_exc = exc
                self.close()
                time.sleep(1.0 * (attempt + 1))
        raise BridgeTimeoutError(f"cmd {line!r} failed after {retries} retries: {last_exc}")

    # ─── High-level Verbs ───

    def ping(self) -> str:
        return self._raw_cmd("ping")

    def info(self) -> str:
        return self._raw_cmd("info")

    def state(self) -> Dict[str, str]:
        raw = self._raw_cmd("state")
        if not raw.startswith("OK "):
            return {}
        kv = {}
        for tok in raw[3:].split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        return kv

    def pollstat(self) -> Dict[str, str]:
        """Read pollstat for wedge detection.

        Returns dict with keys: calls, accepts, hasclient, cclog, anlz, trace.
        """
        raw = self._raw_cmd("pollstat")
        if not raw.startswith("OK "):
            return {}
        kv = {}
        for tok in raw[3:].split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv[k] = v
        return kv

    def is_wedged(self) -> bool:
        """Check if bridge is wedged via pollstat.

        Wedged = port LISTENs (hasclient=1) but no command replies.
        We detect this by checking if pollstat shows hasclient=1 but
        a subsequent ping fails or times out.
        """
        try:
            ps = self.pollstat()
            if ps.get("hasclient") == "1":
                # Verify with a lightweight ping
                ping_reply = self._raw_cmd("ping", retries=1)
                if not ping_reply.startswith("OK"):
                    return True
            return False
        except (BridgeTimeoutError, OSError):
            return True

    def wait_for_flag(
        self,
        flag_name: str,
        expected_value: str,
        timeout: float = 10.0,
        verb: str = "info"
    ) -> bool:
        """Wait for a flag in `info` or `state` to reach expected_value.

        Reads `info` by default (carries `anlz`, `cclog`); `state` does not.
        This is the verification pattern from harness.py — discard the
        measurement if the flag didn't actually switch.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            raw = self._raw_cmd(verb)
            if not raw.startswith("OK "):
                time.sleep(0.1)
                continue
            for tok in raw[3:].split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    if k == flag_name and v == expected_value:
                        return True
            time.sleep(0.1)
        return False

    def step_to(self, target: int, timeout: float = 60.0) -> int:
        """Advance to at least `target` frames, waiting for frame counter."""
        base = self.frame_no()
        if base is None:
            raise BridgeError("state gave no frame number")
        if target <= base:
            return base
        self._raw_cmd(f"step {target - base}")
        deadline = time.time() + timeout
        while time.time() < deadline:
            cur = self.frame_no()
            if cur is not None and cur >= target:
                return cur
        raise BridgeTimeoutError(f"step_to({target}) timed out at frame {self.frame_no()}")

    def frame_no(self) -> Optional[int]:
        st = self.state()
        if "frame" in st:
            return int(st["frame"])
        return None

    def core_ticks(self) -> Optional[int]:
        st = self.state()
        if "core" in st:
            return int(st["core"])
        return None

    def advance_mode(self) -> Optional[int]:
        st = self.state()
        if "adv_mode" in st:
            return int(st["adv_mode"])
        return None

    # ─── Debug Verbs ───

    def rot(self, mode: int) -> str:
        if mode not in (0, 1):
            raise ValueError("rot mode must be 0 or 1")
        return self._raw_cmd(f"rot {mode}")

    def pause(self) -> str:
        return self._raw_cmd("pause")

    def run(self) -> str:
        return self._raw_cmd("run")

    def step(self, frames: int = 1) -> str:
        return self._raw_cmd(f"step {frames}")

    def fbsize(self, screen: int) -> tuple[int, int, int]:
        raw = self._raw_cmd(f"fbsize {screen}")
        if not raw.startswith("OK"):
            raise BridgeError(f"fbsize said: {raw}")
        _, w, h, total = raw.split()
        return int(w), int(h), int(total)

    def fbrd(self, screen: int, offset: int, length: int) -> bytes:
        """Read one framebuffer chunk, with double-buffer settling.

        Uses the same settling logic as netfb.py: reads even-indexed
        chunks until they agree, to handle gfxSwapBuffers() alternating
        between two buffers while paused.
        """
        wants = length
        reads = []
        for _ in range(6):
            reply = self._raw_cmd(f"fbrd {screen} {offset} {wants}")
            if reply.startswith("ERR"):
                raise BridgeError(f"fbrd at {offset} said: {reply}")
            got_off, got_len, hexdata = reply.split(" ", 2)
            if int(got_off) != offset or int(got_len) != wants:
                raise BridgeError(f"out-of-order chunk: asked {offset}/{wants}, got {got_off}/{got_len}")
            reads.append(hexdata.strip())

        even = reads[0::2]
        if len(even) >= 2 and len(set(even)) == 1:
            import binascii
            return binascii.unhexlify(reads[0].strip())

        raise BridgeError(
            f"chunk {offset} never settled after {len(reads)} reads "
            f"({len(set(reads))} distinct values). "
            f"Pause the game first (START on device, or `pause` verb)."
        )

    def fbdump(self, screen: int = 0) -> tuple[int, int, bytes]:
        """Dump entire framebuffer as PNG-ready bytes.

        Uses fbsize then fbrd in chunks. Returns (width, height, raw_rgb565_bytes).
        """
        w, h, total = self.fbsize(screen)
        buf = bytearray()
        chunk = 1024
        offset = 0
        while offset < total:
            want = min(chunk, total - offset)
            buf += self.fbrd(screen, offset, want)
            offset += want
        return w, h, bytes(buf)

    def input(self, mask: int) -> str:
        if mask > 0xFFFF:
            raise ValueError("mask must be <= 0xFFFF")
        return self._raw_cmd(f"input {mask:04X}")

    def tap(self, mask: int, frames: int = 2) -> str:
        if mask > 0xFFFF:
            raise ValueError("mask must be <= 0xFFFF")
        if frames < 2 or frames > 600:
            raise ValueError("frames must be 2..600")
        return self._raw_cmd(f"tap {mask:04X} {frames}")

    def prof(self) -> str:
        return self._raw_cmd("prof")

    def cclog(self, mode: int) -> str:
        if mode not in (0, 1):
            raise ValueError("cclog mode must be 0 or 1")
        return self._raw_cmd(f"cclog {mode}")

    def anlz(self, mode: int) -> str:
        if mode not in (0, 1):
            raise ValueError("anlz mode must be 0 or 1")
        return self._raw_cmd(f"anlz {mode}")

    def trace(self, mode: int) -> str:
        if mode not in (0, 1):
            raise ValueError("trace mode must be 0 or 1")
        return self._raw_cmd(f"trace {mode}")

    # ─── Context Manager ───

    def __enter__(self) -> "Bridge":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        self.close()
        return False


def rgb565_to_png(w: int, h: int, buf: bytes) -> bytes:
    """Encode RGB565 buffer as minimal PNG (for netfb.py compatibility)."""
    import zlib
    import binascii
    import struct

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", binascii.crc32(body) & 0xFFFFFFFF))

    rows = []
    for y in range(h):
        row = bytearray()
        base = y * w * 2
        for x in range(w):
            v = struct.unpack_from("<H", buf, base + x * 2)[0]
            r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r5 << 3) | (r5 >> 2),
                          (g6 << 2) | (g6 >> 4),
                          (b5 << 3) | (b5 >> 2)))
        rows.append(bytes(row))

    raw = b"".join(b"\x00" + row for row in rows)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    return png