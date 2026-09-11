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

    def open_data(rcvbuf=None, timeout=5):
        """A small receive buffer is what makes backpressure reachable. With the
        default, the device's own multi-megabyte send buffer swallows the whole
        burst, the ring never fills, and the drop paths go untested — and any
        backlog a slow reader has to chew through is contiguous, so an in-band
        gap would be thousands of records away."""
        so = socket.socket()
        if rcvbuf:
            so.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        so.settimeout(timeout)
        so.connect(("127.0.0.1", stream_port))
        return so

    data = open_data()
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
    data.close(); time.sleep(0.3)
    data = open_data(rcvbuf=2048)
    gen = subprocess.Popen(["cangen", IFACE, "-g", "0", "-n", "60000"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2.0)          # do not read the data socket at all
    gen.wait(timeout=60)
    time.sleep(0.5)
    dropped = int(c.q("SYSTem:STReam:DROPped?"))
    captured = int(c.q("SYSTem:STReam:COUNt?"))
    check("daemon survived the burst", proc.poll() is None)
    check("burst produced counted drops", dropped > 0, f"dropped={dropped}")
    check("capture kept running through the burst", captured > len(sent),
          f"captured={captured}")

    # In-band gap reporting: loss must arrive inside the records, at the point
    # it happened, rather than having to be inferred or polled for afterwards.
    #
    # Marker frames are sent AFTER the burst on purpose. Everything already
    # buffered in the socket predates the overflow and is contiguous, so a gap
    # is only visible once the reader is past that backlog — the markers are
    # how the test knows it has got there.
    data.close(); time.sleep(0.3)
    data = open_data(rcvbuf=2048, timeout=10)
    # How much traffic it takes to overflow depends on how far the kernel has
    # auto-tuned the device's send buffer, which varies by machine and by run.
    # Burst until the device actually reports loss rather than guessing a
    # number that happens to work here.
    before = int(c.q("SYSTem:STReam:DROPped?"))
    for _ in range(5):
        gen = subprocess.Popen(["cangen", IFACE, "-g", "0", "-n", "60000"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2.0)                   # do not read: force the ring to overflow
        gen.wait(timeout=60)
        time.sleep(0.3)
        if int(c.q("SYSTem:STReam:DROPped?")) > before:
            break
    check("burst overflowed the ring",
          int(c.q("SYSTem:STReam:DROPped?")) > before,
          "no loss even after repeated bursts")
    # Extended (29-bit) identifiers, because cangen emits random 11-bit ones
    # and would otherwise collide with the markers — matching a cangen frame
    # from early in the backlog, which legitimately predates the loss.
    # Drain what the burst left queued before marking. Immediately after an
    # overflow the ring is still full, so a marker sent now would itself be
    # dropped and never captured — the backlog has to clear first.
    data.settimeout(1.0)
    quiet_since = time.time()
    pend = b""
    while time.time() - quiet_since < 1.0 and time.time() - quiet_since < 30:
        try:
            d = data.recv(262144)
        except socket.timeout:
            break
        if not d:
            break
        pend = b""          # backlog content is not interesting, only that it clears
        quiet_since = time.time()

    # Extended (29-bit) identifiers, because cangen emits random 11-bit ones and
    # would otherwise collide — matching a cangen frame from early in the
    # backlog, which legitimately predates the loss.
    MARKER = 0x1FFFFF00
    for i in range(5):
        subprocess.run(["cansend", IFACE, f"{MARKER + i:08X}#C0FFEE"], check=True)

    marker_drop = None
    deadline = time.time() + 15
    while marker_drop is None and time.time() < deadline:
        try:
            d = data.recv(262144)
        except socket.timeout:
            break
        if not d:
            break
        pend += d
        while len(pend) >= STRIDE:
            rec, pend = pend[:STRIDE], pend[STRIDE:]
            _ts, dropped, can_id, _ln, flags, payload = unpack(rec)
            if (flags & 0x01) and MARKER <= can_id < MARKER + 5 and payload == b"\xc0\xff\xee":
                marker_drop = dropped
                break

    check("loss is reported in-band, not inferred",
          marker_drop is not None and marker_drop > 0,
          f"marker record carried dropped={marker_drop}")

    data.close(); time.sleep(0.4)
    c.w("SYSTem:STReam:STOP")
    c.w("CAN:CLOSe")
    check("clean shutdown, no errors", c.q("SYSTem:ERRor?").startswith("0,"))
finally:
    proc.kill(); proc.wait()

print(f"\n{passes} passed, {len(fails)} failed")
if fails: print("FAILED:", ", ".join(fails)); sys.exit(1)
print("ALL PASS")
