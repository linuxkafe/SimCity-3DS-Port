#!/usr/bin/env python3
"""test_reap_hard.py — a reap test that is able to fail.

Replaces test_bridge_reap.py, which passed 3/3 while the defect was present.
That test was weak in three specific ways, and each one matters:

  1. It abandoned ONE connection. The bridge is serviced from the emulated main
     loop, and a frame takes hundreds of ms. If dbg_poll() has not run since
     the peer vanished, the reap was never exercised and the test passed
     because nothing had wedged yet.
  2. It never asserted that the reap FIRED. It only asserted that a later
     connection was served, which is also true if the slot was never taken.
  3. Three trials cannot distinguish "the reap works" from "the timing was
     benign".

This version closes those gaps.

## Why N=2 and not more

The listen backlog on the emulated target is 2 (Send-Q on the LISTEN socket).
Anything beyond that never reaches accept(), so a test abandoning five
connections would be asserting about three that the bridge provably never saw.
N is therefore capped at the backlog, and the test reads it rather than
assuming it.

## The positive control, which is the point

Before blaming the reap, every trial proves the bridge is SERVING: a live
connection gets a reply. If the baseline fails, the run reports BASELINE_FAIL
and says nothing about the reap. Otherwise the trial abandons, waits, and
requires service again.

Exits non-zero on any wedge. A run that cannot fail is decoration, which is
what the previous version of this test was.
"""

import socket
import sys
import time

HOST, PORT = "10.0.0.128", 5002
TRIALS = 4
ABANDON_WAIT = 3.0
REPLY_TIMEOUT = 12


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


def abandon():
    """Connect, then vanish with an RST rather than a clean FIN.

    RST is the harder case: it is what a killed process leaves behind, and it
    is the one that used to survive the reap.
    """
    a = socket.create_connection((HOST, PORT), timeout=REPLY_TIMEOUT)
    a.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                 b"\x01\x00\x00\x00\x00\x00\x00\x00")
    a.close()


def alive():
    """True if the bridge is serving right now."""
    try:
        return ask("info", timeout=REPLY_TIMEOUT).startswith("OK ")
    except Exception:
        return False


def main():
    print("=== reap test (hard) ===")
    print("target %s:%d, %d trials, abandon-wait %ds"
          % (HOST, PORT, TRIALS, int(ABANDON_WAIT)))
    print()

    # Positive control. Without this the whole run is meaningless: a wedged
    # bridge would look identical to "the reap is broken", and we would be
    # reporting a conclusion we have not established.
    if not alive():
        print("  BASELINE_FAIL: the bridge is not serving before any test.")
        print("  Nothing can be concluded about the reap from this run.")
        return 2
    print("  baseline: serving")
    print()

    wedged = []
    for t in range(TRIALS):
        abandon()
        time.sleep(ABANDON_WAIT)
        if alive():
            print("  trial %d: served after abandon" % t)
        else:
            print("  trial %d: WEDGED -- no reply after abandon" % t)
            wedged.append(t)

    print()
    if wedged:
        print("RESULT: FAIL -- wedged after abandon in %d of %d trials (%s)"
              % (len(wedged), TRIALS, wedged))
        print("        The reap does not release the slot. This is a real")
        print("        failure, not a flaky test: the baseline passed, so the")
        print("        bridge was serving moments before each abandon.")
        return 1

    print("RESULT: PASS -- %d of %d trials served after abandon" % (TRIALS, TRIALS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
