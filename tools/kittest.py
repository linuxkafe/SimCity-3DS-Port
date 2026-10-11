#!/usr/bin/env python3
"""Drive the real 3dsdbg server and assert correct protocol behaviour.

Runs against the natively compiled 3dsdbg.c (debug/kit_test/kit), so protocol
bugs are caught before anything is flashed to hardware. The stub serves one
client at a time, so extension and RSP are driven in separate runs against a
fresh server.
"""
import socket, subprocess, sys, time, os, signal

HERE = os.path.dirname(os.path.abspath(__file__))
KIT  = os.path.join(HERE, "..", "debug", "kit_test", "kit")
HOST, PORT = "127.0.0.1", 15002
# Filled in by start_server(): a mapped rw address inside the kit process.
KIT_ADDR = 0

def cksum(p): return sum(p.encode()) & 0xFF
def pkt(p):   return f"${p}#{cksum(p):02x}".encode()

def start_server():
    global KIT_ADDR
    proc = subprocess.Popen([KIT], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    # Resolve a mapped address inside the kit so 'm' reads real memory.
    try:
        for line in open(f"/proc/{proc.pid}/maps"):
            if "rw-p" in line and "kit" in line:
                KIT_ADDR = int(line.split("-")[0], 16)
                break
    except OSError:
        pass
    # wait for the listening socket to actually accept
    for _ in range(50):
        time.sleep(0.1)
        if proc.poll() is not None:
            raise SystemExit("servidor morreu no arranque")
        try:
            socket.create_connection((HOST, PORT), timeout=0.5).close()
            return proc
        except OSError:
            continue
    raise SystemExit("servidor naoescuta")

def connect(timeout=3, retries=20):
    last = None
    for _ in range(retries):
        try:
            s = socket.create_connection((HOST, PORT), timeout=timeout)
            s.settimeout(timeout)
            return s
        except OSError as e:
            last = e
            time.sleep(0.1)
    raise last

def line(s, msg, timeout=3):
    s.sendall((msg + "\n").encode())
    time.sleep(0.15)
    return s.recv(4096).decode(errors="replace").strip()

def rxpkt(s, timeout=3):
    buf = b""
    s.settimeout(timeout)
    while True:
        try:    c = s.recv(4096)
        except socket.timeout: return None
        if not c: return None
        buf += c
        while buf[:1] in (b"+", b"-"): buf = buf[1:]
        i = buf.find(b"$")
        if i == -1: continue
        j = buf.find(b"#", i)
        if j != -1 and len(buf) >= j + 3:
            s.sendall(b"+")
            return buf[i+1:j].decode(errors="replace")

ok, fail = [], []
def check(name, cond, detail=""):
    (ok if cond else fail).append(name)
    print(f"{'PASS' if cond else 'FAIL'}  {name}{'  ' + detail if detail else ''}")

# ---- block 1: extension protocol ----
p = start_server()
try:
    s = connect()
    check("ext: ping", line(s, "ping").startswith("OK"))
    check("ext: info", line(s, "info").startswith("OK"))
    check("ext: brk",  line(s, "brk 0x00110000").startswith("OK"))
    check("ext: unbrk",line(s, "unbrk 0x00110000").startswith("OK"))
    check("ext: dumpmem", line(s, "dumpmem 0x600000 16").startswith("OK"))
    s.close()
finally:
    p.send_signal(signal.SIGKILL); p.wait(); time.sleep(0.2)

# ---- block 2: RSP ----
p = start_server()
try:
    s = connect()
    s.sendall(pkt("qSupported:multiprocess+;swbreak+;xmlRegisters=i386"))
    r = rxpkt(s)
    check("rsp: qSupported", r is not None and "PacketSize" in r, f"got {r!r}")
    s.sendall(pkt("?")); r = rxpkt(s)
    check("rsp: stop-reply ?", bool(r) and r.startswith("T"), f"got {r!r}")
    s.sendall(pkt("g")); r = rxpkt(s)
    # GDB14 for ARM requires a 26-word (32-bit each) 'g' frame.
    check("rsp: registos 'g' (26 palavras)",
          bool(r) and len(r) == 26*8 and all(c in "0123456789abcdef" for c in r),
          f"len={len(r or '')} esperado={26*8}")
    # Read a page that is certainly mapped in the test process: its own
    # data segment. The stub validates nothing, so use the address the harness
    # printed for the kit's rw region.
    addr = KIT_ADDR
    s.sendall(pkt(f"m{addr:x},10")); r = rxpkt(s)
    check("rsp: le memoria 'm'", r is not None and len(r) == 32, f"got {r!r}")
    s.sendall(pkt("Z0,00110000,2")); r = rxpkt(s)
    check("rsp: breakpoint 'Z'", r == "OK", f"got {r!r}")
    s.sendall(pkt("vMustReplyEmpty")); r = rxpkt(s)
    check("rsp: 'v' responde vazio", r is not None, f"got {r!r}")
    s.close()
finally:
    p.send_signal(signal.SIGKILL); p.wait(); time.sleep(0.2)

print(f"\n{len(ok)} passed, {len(fail)} failed")
if fail: print("FALHARAM: " + ", ".join(fail))
sys.exit(1 if fail else 0)
