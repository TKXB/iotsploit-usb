#!/usr/bin/env python3
"""Protocol smoke suite for the iotsploit-usb SCPI/TCP daemon.

Transport-level coverage the C unit tests cannot reach: real sockets, real
response framing, real reconnects. The same checks run against any daemon
build, which is how the Windows port is validated from a Linux box.

    # spawn a daemon on a free port and test it (this is what ctest runs)
    scpi_tcp_smoke.py --daemon build/examples/daemon/usbscpi_daemon

    # or point it at an already-running daemon, local or remote
    scpi_tcp_smoke.py 127.0.0.1 5025
"""
import socket, subprocess, sys, time

proc = None
if sys.argv[1] == "--daemon":
    # Let the OS pick a free port, then hand it to the daemon. Racy in
    # principle; in a test run nothing else is competing for it.
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    HOST, PORT = "127.0.0.1", probe.getsockname()[1]
    probe.close()
    proc = subprocess.Popen([sys.argv[2], HOST, str(PORT)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    deadline = time.time() + 10
    while time.time() < deadline:                 # wait for the listener
        try:
            socket.create_connection((HOST, PORT), timeout=0.5).close()
            break
        except OSError:
            if proc.poll() is not None:
                sys.exit(f"daemon exited early with {proc.returncode}")
            time.sleep(0.1)
    else:
        proc.kill(); sys.exit("daemon never started listening")
else:
    HOST, PORT = sys.argv[1], int(sys.argv[2])

fails, passes = [], 0

def check(name, cond, detail=""):
    global passes
    if cond:
        passes += 1
        print(f"  PASS  {name}")
    else:
        fails.append(name)
        print(f"  FAIL  {name}  {detail}")

class Dev:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=5)
        self.buf = b""
    def close(self): self.s.close()
    def _fill(self):
        d = self.s.recv(65536)
        if not d: raise EOFError("peer closed")
        self.buf += d
    def write(self, cmd):
        self.s.sendall(cmd.encode() + b"\n")
    def read_msg(self):
        # Framing per the protocol: '#' -> definite-length block, else to '\n'.
        while not self.buf: self._fill()
        if self.buf[0:1] == b"#":
            while len(self.buf) < 2: self._fill()
            n = int(self.buf[1:2])
            while len(self.buf) < 2 + n: self._fill()
            ln = int(self.buf[2:2+n])
            head = 2 + n
            while len(self.buf) < head + ln: self._fill()
            payload = self.buf[head:head+ln]
            self.buf = self.buf[head+ln:]
            while not self.buf.startswith(b"\n"):
                if not self.buf: self._fill()
                else: break
            if self.buf.startswith(b"\n"): self.buf = self.buf[1:]
            return payload
        while b"\n" not in self.buf: self._fill()
        line, _, rest = self.buf.partition(b"\n")
        self.buf = rest
        return line
    def query(self, cmd):
        self.write(cmd)
        return self.read_msg()
    def query_multiline(self, cmd):
        """HELP:HEADers? writes one line per pattern and ends with a blank
        line (see host/rust/src/session.rs::query_multiline)."""
        self.write(cmd)
        out = []
        for _ in range(4096):
            line = self.read_msg()
            if line == b"":
                return b"\n".join(out)
            out.append(line)
        raise RuntimeError("multi-line response did not terminate")

print(f"== connecting to {HOST}:{PORT} ==")
d = Dev(HOST, PORT)

check("*IDN?", d.query("*IDN?") == b"IoTSploit,tcp-demo,0001,0.1.0",
      repr(d.buf))

d.write("GPIO:SET 2,1")           # non-query: must send nothing back
check("GPIO:GET? after set", d.query("GPIO:GET? 2") == b"1")
d.write("GPIO:SET 2,0")
check("GPIO:GET? after clear", d.query("GPIO:GET? 2") == b"0")

err = d.query("SYSTem:ERRor?")
check("no errors queued", err.startswith(b"0,"), err)

# multi-command line: the batch fix means a failure must not eat the rest
d.write("GPIO:SET 3,1;GPIO:SET 4,1")
check("batched commands both applied",
      d.query("GPIO:GET? 3") == b"1" and d.query("GPIO:GET? 4") == b"1")

# error path
d.write("NO:SUCH:COMMAND")
err = d.query("SYSTem:ERRor?")
check("bad command queues an error", err.startswith(b"-1"), err)
d.write("*CLS")

# trigger_poll_fetch workflow
d.write("DEMO:SCAN")
deadline = time.time() + 10
done = b"0"
while time.time() < deadline:
    done = d.query("DEMO:SCAN:DONE?")
    if done == b"1": break
    time.sleep(0.2)
check("workflow reaches DONE", done == b"1", done)
cnt = d.query("DEMO:SCAN:COUNt?")
check("workflow count == 8", cnt == b"8", cnt)
row0 = d.query("DEMO:SCAN? 0")
check("workflow row 0 shape", row0.startswith(b'"demo-ap-0"') and row0.count(b",") == 4, row0)
rows_ok = all(d.query(f"DEMO:SCAN? {i}").startswith(f'"demo-ap-{i}"'.encode())
              for i in range(int(cnt)))
check("all workflow rows fetch", rows_ok)

# definite-length binary block -- the path most likely to expose a send() bug
for size in (16, 512, 4096):
    blk = d.query(f"DEMO:DATA? {size}")
    check(f"block read {size}B length", len(blk) == size, f"got {len(blk)}")
    check(f"block read {size}B payload", blk == bytes(i & 0xFF for i in range(size)))

# descriptor discovery
hdrs = d.query_multiline("SYSTem:HELP:HEADers?")
check("HELP:HEADers? lists commands", b"GPIO:SET" in hdrs and b"DEMO:DATA?" in hdrs, hdrs[:80])
desc = d.query("SYSTem:HELP:DESCription?")
check("descriptor has DEV record", b"DEV" in desc, desc[:60])
check("descriptor has CMD records", desc.count(b"CMD") >= 7)
check("descriptor has WF record", b"demo-scan" in desc)
check("descriptor params use designated fields", b"pin" in desc and b"level" in desc)

# reconnect: usbscpi_clear() must leave the next session clean
d.write("DEMO:DATA? 4096")   # leave a reply unread, then drop the link
d.close()
time.sleep(0.3)
d2 = Dev(HOST, PORT)
check("reconnect works", d2.query("*IDN?") == b"IoTSploit,tcp-demo,0001,0.1.0")
check("state clean after reconnect", d2.query("SYSTem:ERRor?").startswith(b"0,"))
d2.close()

if proc:
    proc.kill(); proc.wait()

print(f"\n{passes} passed, {len(fails)} failed")
if fails:
    print("FAILED:", ", ".join(fails)); sys.exit(1)
print("ALL PASS")
