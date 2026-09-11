#!/usr/bin/env python3
"""Data-plane transport tests: glue/usbscpi_stream.c.

Covers the three behaviours a direct implementation gets wrong — backpressure
that stalls the producer, a reconnect that leaves the stream misaligned
forever, and a drain loop that either busy-spins or misses a FIN while idle.

    test_stream_dataplane.py <path-to-stream_testgen>
"""
import os, socket, struct, subprocess, sys, threading, time

GEN = sys.argv[1]
STRIDE, MAGIC = 20, 0x5EC0FFEE
UNTHROTTLED = 10_000_000   # period < sleep granularity, so the producer free-runs
fails, passes = [], 0

def check(name, cond, detail=""):
    global passes
    if cond:
        passes += 1; print(f"  PASS  {name}")
    else:
        fails.append(name); print(f"  FAIL  {name}  {detail}")

class Gen:
    """The synthetic producer, plus a reader for its disconnect reports."""
    def __init__(self, rate):
        self.port = self._free_port()
        self.p = subprocess.Popen([GEN, "127.0.0.1", str(self.port), str(rate)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, bufsize=1)
        self.lines = []
        threading.Thread(target=self._pump, daemon=True).start()
        self._wait_listening()
    @staticmethod
    def _free_port():
        s = socket.socket(); s.bind(("127.0.0.1", 0))
        p = s.getsockname()[1]; s.close(); return p
    def _pump(self):
        for line in self.p.stdout:
            self.lines.append(line.strip())
    def _wait_listening(self):
        for _ in range(100):
            if any("listening" in l for l in self.lines): return
            if self.p.poll() is not None:
                raise SystemExit(f"testgen exited early: {self.lines}")
            time.sleep(0.05)
        raise SystemExit("testgen never reported listening")
    def torn(self):
        vals = [int(l.split("torn=")[1].split()[0]) for l in self.lines if "torn=" in l]
        return vals[-1] if vals else 0
    def dropped(self):
        vals = [int(l.split("dropped=")[1].split()[0]) for l in self.lines if "dropped=" in l]
        return vals[-1] if vals else 0
    def connect(self, rcvbuf=None):
        s = socket.socket()
        if rcvbuf: s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        s.settimeout(5); s.connect(("127.0.0.1", self.port)); return s
    def cpu_seconds(self):
        with open(f"/proc/{self.p.pid}/stat") as f: parts = f.read().split()
        return (int(parts[13]) + int(parts[14])) / os.sysconf("SC_CLK_TCK")
    def stop(self):
        self.p.kill(); self.p.wait()

def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        d = sock.recv(n - len(buf))
        if not d: raise EOFError
        buf += d
    return buf

def records(blob):
    return [struct.unpack("<IIIII", blob[i:i+STRIDE]) for i in range(0, len(blob), STRIDE)]

# ---- 1. records arrive intact, aligned, in order -------------------------
g = Gen(rate=20000)
s = g.connect()
recs = records(read_exact(s, STRIDE * 200))
check("records carry the magic", all(r[1] == MAGIC for r in recs),
      f"{recs[:2]}")
check("records are not torn", all(r[0] == r[2] for r in recs))
check("sequence is strictly increasing",
      all(b[0] > a[0] for a, b in zip(recs, recs[1:])))
s.close(); time.sleep(0.2)

# ---- 2. a stalled consumer must not stall the producer -------------------
# Unthrottled: a rate-limited producer cannot fill the device's own send buffer
# (megabytes by default), so send() would never block and neither backpressure
# nor realignment would ever be reached.
g.stop()
g = Gen(rate=UNTHROTTLED)
s = g.connect(rcvbuf=2048)
read_exact(s, STRIDE)
time.sleep(1.0)                       # stop reading; socket + ring both fill
after = records(read_exact(s, STRIDE * 4))
check("still aligned after backpressure", all(r[1] == MAGIC for r in after))
s.close(); time.sleep(0.4)
# Records read straight after the stall are the ones that were already
# buffered, so their sequence is legitimately contiguous — the proof the
# producer kept running is that it had to drop, which only overflow causes.
check("producer kept running while consumer stalled", g.dropped() > 0,
      f"dropped={g.dropped()}")

# ---- 3. a consumer that dies mid-record must not misalign the next one ----
aligned_after_reconnect = True
for _ in range(6):
    s = g.connect(rcvbuf=2048)
    read_exact(s, STRIDE)             # get the stream flowing
    time.sleep(0.5)                   # send buffer fills, then ring fills                   # let send() block part-way through a record
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.close()                         # abrupt RST, mid-record
    time.sleep(0.2)
    s2 = g.connect()
    r = records(read_exact(s2, STRIDE * 4))
    if not all(x[1] == MAGIC and x[0] == x[2] for x in r):
        aligned_after_reconnect = False
    s2.close(); time.sleep(0.15)
    if g.torn() >= 1: break
check("reconnect after a mid-record death stays aligned", aligned_after_reconnect)
check("glue counted the torn record", g.torn() >= 1, f"torn={g.torn()}")
g.stop()

# ---- 4. idle: no busy-spin, and a FIN is still noticed -------------------
g = Gen(rate=0)                       # produces nothing at all
s = g.connect()
t0, c0 = time.time(), g.cpu_seconds()
time.sleep(2.0)
busy = (g.cpu_seconds() - c0) / (time.time() - t0)
check("idle stream does not busy-spin", busy < 0.15, f"{busy*100:.1f}% of a core")

s.close()
noticed = False
for _ in range(40):                   # poll timeout is 5 ms; allow generous slack
    if any("disconnect" in l for l in g.lines): noticed = True; break
    time.sleep(0.025)
check("disconnect noticed while idle", noticed)
g.stop()

print(f"\n{passes} passed, {len(fails)} failed")
if fails:
    print("FAILED:", ", ".join(fails)); sys.exit(1)
print("ALL PASS")
