"""Session ownership, immediate STOP and partial-upload cleanup over real TCP."""
import socket
import subprocess
import sys
import time
with socket.socket() as probe:
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
proc = subprocess.Popen([sys.argv[1], str(port)])
def connect():
    deadline = time.monotonic() + 5
    while True:
        try:
            s = socket.create_connection(('127.0.0.1', port), timeout=1)
            return s, s.makefile('rb')
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.02)
def query(s, f, cmd):
    s.sendall(cmd.encode() + b'\n')
    return f.readline().strip()
try:
    s, f = connect()
    s.sendall(b'DUCK:RUN\nDUCK:STOP\n')
    assert query(s, f, 'DUCK:STATe?') == b'4'
    s.sendall(b'DUCK:RUN\n')
    assert query(s, f, 'DUCK:STATe?') == b'1'
    f.close(); s.close()
    s, f = connect()
    assert query(s, f, 'DUCK:STATe?') == b'4', 'disconnect must cancel run'
    s.sendall(b'DUCK:UPLoad:STARt 5\nDATA:WRITE #15EN')
    f.close(); s.close()
    s, f = connect()
    assert query(s, f, 'DUCK:UPLoad:STATe?') == b'0', 'reservation must be released'
    s.sendall(b'DATA:WRITE #15ENTER\n')
    assert query(s, f, 'DUCK:UPLoad:STATe?') == b'3', 'new session needs its own reservation'
    s.sendall(b'DUCK:UPLoad:STARt 5\nDATA:WRITE #15ENTER\n')
    assert query(s, f, 'DUCK:UPLoad:STATe?') == b'2'
    f.close(); s.close()
    print('HID socket lifecycle regressions passed')
finally:
    proc.terminate()
    proc.wait(timeout=5)
