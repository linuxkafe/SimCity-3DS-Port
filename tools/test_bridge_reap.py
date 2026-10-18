#!/usr/bin/env python3
"""test_bridge_reap.py — the control test for the CLOSE-WAIT fix.

T034 / T027. This exists because the fix it tests was written without being
tested, which is the exact failure T033 was created to prevent.

The bug it reproduces: dbgk_poll() held g_client_fd forever once a peer
vanished. A non-blocking recv() on such a socket returns EAGAIN indefinitely,
because the OS clears the RST silently when no data is pending. The port kept
LISTENing, so a naive reachability check passed, while the bridge was silent
and every new connection was refused.

The fix polls g_client_fd for POLLHUP/POLLERR before accepting, and frees the
slot when the peer is gone.

The test:
  1. Open a connection and ABANDON it without closing -- the peer's process
     exits, so the FD closes and the bridge sees FIN/RST with nobody left.
  2. Immediately open a second connection and require a reply.

Before the fix, step 2 times out forever. After it, step 2 answers.

Three trials, because one green is one sample. Exits non-zero if any trial
hangs, which is the only assertion that matters here.
"""

import socket
import sys
import time

HOST, PORT = "10.0.0.128", 5002
TRIALS = 3
ABANDON_WAIT = 2.0
REPLY_TIMEOUT = 15


def ask(cmd, timeout=REPLY_TIMEOUT):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    s.settimeout(timeout)
    try:
        f = s.makefile("rwb")
        f.write((cmd + "\n").encode())
        f.flush()
        return f.readline().decode().strip()
    finally:
        try:
            s.close()
        except OSError:
            pass


def main():
    print("=== bridge reap control test ===")
    print("target %s:%d, %d trials" % (HOST, PORT, TRIALS))
    print()

    try:
        info = ask("info")
    except Exception as exc:
        print("  ! target not reachable: %s: %s" % (type(exc).__name__, exc))
        return 2
    print("  baseline info: %s" % info)
    print()

    failures = 0
    for t in range(TRIALS):
        # 1. Abandon a connection: connect, then drop it without close().
        a = socket.create_connection((HOST, PORT), timeout=REPLY_TIMEOUT)
        a.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     b"\x01\x00\x00\x00\x00\x00\x00\x00")  # RST on close
        a.close()
        print("  trial %d: connection abandoned (RST), waiting %ds"
              % (t, int(ABANDON_WAIT)))
        time.sleep(ABANDON_WAIT)

        # 2. A fresh connection must still be served.
        try:
            reply = ask("info", timeout=REPLY_TIMEOUT)
            ok = reply.startswith("OK ")
        except Exception as exc:
            reply = "%s: %s" % (type(exc).__name__, exc)
            ok = False

        if ok:
            print("  trial %d: PASS -- bridge answered after the abandoned peer"
                  % t)
        else:
            print("  trial %d: FAIL -- bridge silent after the abandoned peer: %s"
                  % (t, reply))
            failures += 1

    print()
    if failures:
        print("RESULT: FAIL -- %d of %d trials left the bridge wedged"
              % (failures, TRIALS))
        return 1
    print("RESULT: PASS -- %d of %d trials recovered" % (TRIALS, TRIALS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
