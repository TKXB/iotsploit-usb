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

d.write("GPIO 2,1")               # non-query: must send nothing back
check("GPIO? after set", d.query("GPIO? 2") == b"1")
d.write("GPIO 2,0")
check("GPIO? after clear", d.query("GPIO? 2") == b"0")

err = d.query("SYSTem:ERRor?")
check("no errors queued", err.startswith(b"0,"), err)

# multi-command line: the batch fix means a failure must not eat the rest
d.write("GPIO 3,1;GPIO 4,1")
check("batched commands both applied",
      d.query("GPIO? 3") == b"1" and d.query("GPIO? 4") == b"1")

# names from before the command standard still work, undescribed
d.write("GPIO:SET 5,1")
check("old GPIO:SET/GPIO:GET? aliases", d.query("GPIO:GET? 5") == b"1")

# error path
d.write("NO:SUCH:COMMAND")
err = d.query("SYSTem:ERRor?")
check("bad command queues an error", err.startswith(b"-1"), err)
d.write("*CLS")

def wait_state(want, timeout=10):
    deadline = time.time() + timeout
    state = b""
    while time.time() < deadline:
        state = d.query("DEMO:SCAN:STATe?")
        if state == want: break
        time.sleep(0.2)
    return state

# job: STARt -> STATe? RUNNING ... DONE -> COUNt? -> FETCh?
check("job starts IDLE", d.query("DEMO:SCAN:STATe?") == b"IDLE")
d.write("DEMO:SCAN:STARt")
check("job is RUNNING", d.query("DEMO:SCAN:STATe?") == b"RUNNING")
state = wait_state(b"DONE")
check("job reaches DONE", state == b"DONE", state)
cnt = d.query("DEMO:SCAN:COUNt?")
check("job count == 8", cnt == b"8", cnt)
row0 = d.query("DEMO:SCAN:FETCh? 0")
check("job row 0 shape", row0.startswith(b'"demo-ap-0"') and row0.count(b",") == 4, row0)
rows_ok = all(d.query(f"DEMO:SCAN:FETCh? {i}").startswith(f'"demo-ap-{i}"'.encode())
              for i in range(int(cnt)))
check("all job rows fetch", rows_ok)
d.write("DEMO:SCAN:CLEar")
check("CLEar forgets results", d.query("DEMO:SCAN:COUNt?") == b"0" and
      d.query("DEMO:SCAN:STATe?") == b"IDLE")

# the old trigger/done/fetch names drive the same job
d.write("DEMO:SCAN")
deadline = time.time() + 10
done = b"0"
while time.time() < deadline:
    done = d.query("DEMO:SCAN:DONE?")
    if done == b"1": break
    time.sleep(0.2)
check("old DEMO:SCAN/DONE? aliases", done == b"1", done)
check("old DEMO:SCAN? alias", d.query("DEMO:SCAN? 1").startswith(b'"demo-ap-1"'))

# definite-length binary block -- the path most likely to expose a send() bug
for size in (16, 512, 4096):
    blk = d.query(f"DEMO:DATA? {size}")
    check(f"block read {size}B length", len(blk) == size, f"got {len(blk)}")
    check(f"block read {size}B payload", blk == bytes(i & 0xFF for i in range(size)))

# descriptor discovery
hdrs = d.query_multiline("SYSTem:HELP:HEADers?")
check("HELP:HEADers? lists commands", b"DEMO:SCAN:STARt" in hdrs and b"DEMO:DATA?" in hdrs, hdrs[:80])
desc = d.query("SYSTem:HELP:DESCription?")
check("descriptor has DEV record", b"DEV" in desc, desc[:60])
check("descriptor has CMD records", desc.count(b"CMD ") == 9)
check("aliases are not described", b"CMD GPIO:SET" not in desc and b"CMD DEMO:SCAN?" not in desc)
check("descriptor has WF record", b"WF demo-scan" in desc and b"done=DEMO:SCAN:STATe?:DONE" in desc)
check("descriptor params", b"param=pin:u32:req param=value:bool:req" in desc)

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
