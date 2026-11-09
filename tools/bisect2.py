import socket, time, re, sys, subprocess

BIN = "/home/deck/simcity-test/simcity_bisect.3dsx"
RSKIP = int(sys.argv[1])
DUR = int(sys.argv[2]) if len(sys.argv) > 2 else 45

def verb(cmd, t=25):
    s = socket.create_connection(("127.0.0.1", 5002), timeout=t)
    s.settimeout(t)
    s.sendall((cmd + "\n").encode())
    d = b""
    while b"\n" not in d:
        c = s.recv(4096)
        if not c:
            break
        d += c
    s.close()
    return d.decode(errors="replace").strip()

def state():
    o = {}
    for k, v in re.findall(r"(\w+)=(\S+)", verb("state")):
        if v.startswith("0x"):
            continue
        try:
            o[k] = int(v)
        except ValueError:
            pass
    return o

subprocess.Popen(["/home/deck/simcity-test/simcity-headless", "--max-seconds", "300", BIN],
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(25)

print("info:", verb("info"))
print("rskip set:", verb("rskip %d" % RSKIP))
print("info:", verb("info"))

B = 0x8000
for i in range(90):
    verb("tap %04X 6" % B, t=8)
    time.sleep(0.30)

a = state()
t0 = time.time()
time.sleep(DUR)
b = state()
dt = time.time() - t0
df = max(b["frame"] - a["frame"], 1)
di = b["instructions"] - a["instructions"]
print("RESULT rskip=%d frames=%d hostfps=%.2f core=%.3fs loop=%.3fs render=%.1fms instr/frame=%d hostx=%.1f" % (
    RSKIP, df, df/dt, b["core"]/1e6, b["loop"]/1e6, b["render"]/1e3, di//df,
    (b["loop"]/1e6)*df/dt))
print("done")