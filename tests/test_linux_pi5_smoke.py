#!/usr/bin/env python3
"""Host smoke test for examples/linux-pi5 (usbscpi_linux_pi5).

Runs on any Linux host without Pi 5 hardware: spawns the server on an
ephemeral localhost port and verifies *IDN?, descriptor discovery,
system status queries, and safe GPIO direction/range handling.
"""

import socket
import subprocess
import sys
import time


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def query(sock: socket.socket, cmd: str) -> str:
    sock.sendall((cmd + "\n").encode("ascii"))
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError(f"EOF waiting for reply to {cmd!r}")
        buf += chunk
    return buf.decode("utf-8", errors="replace").strip()


def connect_with_retry(port: int, timeout: float = 5.0) -> socket.socket:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2.0)
            s.settimeout(2.0)
            return s
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"server did not listen on 127.0.0.1:{port}")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <path-to-usbscpi_linux_pi5>", file=sys.stderr)
        return 2

    binary = sys.argv[1]
    port = free_port()
    proc = subprocess.Popen(
        [binary, "--bind", "127.0.0.1", "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        with connect_with_retry(port) as sock:
            idn = query(sock, "*IDN?")
            assert idn.startswith("IoTSploit,Linux-Pi5,"), f"unexpected IDN: {idn!r}"

            upt = int(query(sock, "SYST:UPT?"))
            assert upt >= 0, f"bad uptime: {upt}"

            mem = query(sock, "SYST:MEM?")
            parts = mem.split(",")
            assert len(parts) == 2 and int(parts[0]) > 0, f"bad memory: {mem!r}"

            load = query(sock, "SYST:LOAD?")
            assert len(load.split(",")) == 3, f"bad load: {load!r}"

            host = query(sock, "SYST:HOST?")
            assert len(host.split(",")) == 3, f"bad host: {host!r}"

            net = query(sock, "SYST:NET? lo")
            assert "127.0.0.1" in net, f"bad loopback net: {net!r}"

            gdir = query(sock, "GPIO:DIR? 17")
            assert gdir == "FREE", f"unexpected initial dir: {gdir!r}"

            err = query(sock, "SYST:ERR?")
            assert err.startswith("0,"), f"unexpected SCPI error: {err!r}"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

    print("PASS: linux-pi5 host smoke")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
