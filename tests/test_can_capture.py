#!/usr/bin/env python3
"""End-to-end CAN capture over the data plane, against a vcan interface.

    sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
    test_can_capture.py <path-to-usbscpi_can> [vcan0]

Exercises the whole chain: SCPI control plane opens the interface and starts
the capture, records arrive on the separate data socket, and the counters
reconcile. A virtual interface covers everything except real bus timing and
error frames — confirm those on hardware before trusting this alone.
"""
import socket, struct, subprocess, sys, time

BIN   = sys.argv[1]
IFACE = sys.argv[2] if len(sys.argv) > 2 else "vcan0"
STRIDE = 88
fails, passes = [], 0

def check(name, cond, detail=""):
    global passes
    if cond: passes += 1; print(f"  PASS  {name}")
    else: fails.append(name); print(f"  FAIL  {name}  {detail}")

def have_prereqs():
    """Skip rather than fail where the rig has no vcan: this test needs a
    kernel interface and can-utils, neither of which is guaranteed."""
    import shutil, os
    if not os.path.exists(f"/sys/class/net/{IFACE}"):
        print(f"SKIP: {IFACE} does not exist "
              f"(sudo ip link add dev {IFACE} type vcan && sudo ip link set up {IFACE})")
        return False
    for tool in ("cansend", "cangen"):
        if shutil.which(tool) is None:
            print(f"SKIP: {tool} not installed (can-utils)")
            return False
    return True

if not have_prereqs():
    sys.exit(0)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

class Scpi:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5); self.buf = b""
    def q(self, cmd):
        self.s.sendall(cmd.encode() + b"\n")
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
        line, _, self.buf = self.buf.partition(b"\n")
        return line.decode()
    def w(self, cmd):
        self.s.sendall(cmd.encode() + b"\n")

def unpack(rec):
    ts, dropped, can_id, ln, flags, _rsv = struct.unpack_from("<QQIBBH", rec, 0)
    return ts, dropped, can_id, ln, flags, rec[24:24+ln]

scpi_port, stream_port = free_port(), free_port()
proc = subprocess.Popen([BIN, "127.0.0.1", str(scpi_port), str(stream_port)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
time.sleep(1.0)
try:
    c = Scpi("127.0.0.1", scpi_port)
    check("*IDN?", c.q("*IDN?") == "IoTSploit,can-capture,0001,0.1.0")

    fmt = c.q("SYSTem:STReam:FORMat?")
    check("format advertises stride", f"stride={STRIDE}" in fmt, fmt)
    check("format is self-describing", "fields=" in fmt and "can_id" in fmt, fmt)
    check("stream port matches", c.q("SYSTem:STReam:PORT?") == str(stream_port))

    c.w(f'CAN:OPEN "{IFACE}"')
    check("no error after open", c.q("SYSTem:ERRor?").startswith("0,"))
    st = c.q("CAN:STATe?")
    check("interface reported open", f'"{IFACE}",1,0' in st, st)

    c.w("SYSTem:STReam:STARt")
    check("no error after start", c.q("SYSTem:ERRor?").startswith("0,"))

    data = socket.create_connection(("127.0.0.1", stream_port), timeout=5)
    time.sleep(0.3)
    check("attached is visible over SCPI", c.q("CAN:STATe?").endswith(",1,1"),
          c.q("CAN:STATe?"))

    # Known frames, so every field can be checked rather than just counted.
    sent = [(0x100 + i, bytes([i, i ^ 0xFF, 0xAA, 0x55])) for i in range(40)]
    for cid, payload in sent:
        subprocess.run(["cansend", IFACE, f"{cid:03X}#{payload.hex().upper()}"], check=True)
    time.sleep(0.5)

    buf = b""
    data.settimeout(5)
    while len(buf) < STRIDE * len(sent):
        d = data.recv(65536)
        if not d: break
        buf += d
    check("received one record per frame", len(buf) >= STRIDE * len(sent),
          f"{len(buf)//STRIDE} records")

    recs = [unpack(buf[i:i+STRIDE]) for i in range(0, STRIDE*len(sent), STRIDE)]
    check("identifiers match, in order",
          [r[2] for r in recs] == [c_ for c_, _ in sent],
          f"{[hex(r[2]) for r in recs[:4]]}")
    check("payloads match", [r[5] for r in recs] == [p for _, p in sent])
    check("lengths match", all(r[3] == 4 for r in recs))
    check("kernel timestamps look real",
          all(r[0] > 1_600_000_000_000_000 for r in recs), f"{recs[0][0]}")
    check("timestamps are non-decreasing",
          all(b[0] >= a[0] for a, b in zip(recs, recs[1:])))
    check("no drops on a quiet bus", all(r[1] == 0 for r in recs))
    check("capture count agrees", int(c.q("SYSTem:STReam:COUNt?")) >= len(sent))

    # Burst: a stalled consumer must cost counted drops, never a stalled producer.
    gen = subprocess.Popen(["cangen", IFACE, "-g", "0", "-n", "20000"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.0)          # do not read the data socket at all
    gen.wait(timeout=30)
    time.sleep(0.5)
    dropped = int(c.q("SYSTem:STReam:DROPped?"))
    captured = int(c.q("SYSTem:STReam:COUNt?"))
    check("daemon survived the burst", proc.poll() is None)
    check("burst produced counted drops", dropped > 0, f"dropped={dropped}")
    check("capture kept running through the burst", captured > len(sent),
          f"captured={captured}")

    data.close(); time.sleep(0.4)
    c.w("SYSTem:STReam:STOP")
    c.w("CAN:CLOSe")
    check("clean shutdown, no errors", c.q("SYSTem:ERRor?").startswith("0,"))
finally:
    proc.kill(); proc.wait()

print(f"\n{passes} passed, {len(fails)} failed")
if fails: print("FAILED:", ", ".join(fails)); sys.exit(1)
print("ALL PASS")
