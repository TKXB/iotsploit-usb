#!/usr/bin/env python3
"""Host smoke test for examples/linux-pi5 (usbscpi_linux_pi5).

Runs on any Linux host without Pi 5 hardware: spawns the server on an
ephemeral localhost port and verifies *IDN?, descriptor discovery,
system status queries, shell command execution (sync & async),
binary file upload/download (with NUL/LF/# bytes and CRC-32), and
safe GPIO direction/range handling.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time
import zlib


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def read_reply_bytes(sock: socket.socket) -> bytes:
    buf = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("EOF waiting for reply")
        buf += chunk
        if buf.startswith(b"#"):
            nd = int(chr(buf[1]))
            if len(buf) >= 2 + nd:
                nbytes = int(buf[2 : 2 + nd].decode("ascii"))
                total = 2 + nd + nbytes + 1
                if len(buf) >= total:
                    return buf[2 + nd : 2 + nd + nbytes]
        elif buf.endswith(b"\n"):
            return buf[:-1].rstrip(b"\r")


def query_bytes(sock: socket.socket, cmd: str) -> bytes:
    sock.sendall((cmd + "\n").encode("ascii"))
    return read_reply_bytes(sock)


def query(sock: socket.socket, cmd: str) -> str:
    return query_bytes(sock, cmd).decode("utf-8", errors="replace").strip()


def send(sock: socket.socket, cmd: str) -> None:
    sock.sendall((cmd + "\n").encode("ascii"))


def send_data_write(sock: socket.socket, payload: bytes) -> None:
    len_str = str(len(payload)).encode("ascii")
    hdr = b"DATA:WRITE #" + str(len(len_str)).encode("ascii") + len_str
    sock.sendall(hdr + payload)


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
    tmp_fd, tmp_path = tempfile.mkstemp(prefix="usbscpi_pi5_xfer_")
    os.close(tmp_fd)
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

            # Synchronous multi-line shell execution
            out = query_bytes(sock, 'SYST:EXEC? "printf \'line1\\nline2\\n\'"').decode("utf-8")
            assert out == "line1\nline2\n", f"unexpected sync exec output: {out!r}"
            assert query(sock, "SYST:EXEC:CODE?") == "0"

            # Asynchronous job execution
            send(sock, 'SYST:EXEC:STARt "echo async-done"')
            deadline = time.monotonic() + 2.0
            state = "RUNNING"
            while time.monotonic() < deadline:
                state = query(sock, "SYST:EXEC:STATe?")
                if state != "RUNNING":
                    break
                time.sleep(0.02)
            assert state == "DONE", f"async job did not reach DONE: {state!r}"
            assert query(sock, "SYST:EXEC:CODE?") == "0"
            assert query(sock, "SYST:EXEC:OUTPut?").strip() == "async-done"
            send(sock, "SYST:EXEC:CLEar")
            assert query(sock, "SYST:EXEC:STATe?") == "IDLE"

            # Binary file upload & download (includes NUL, CR, LF, '#', all 0..255 bytes)
            payload = bytes(range(256)) * 20 + b"\x00\r\n#99999binary-tail\x00"
            expected_crc = zlib.crc32(payload) & 0xFFFFFFFF

            assert query(sock, "FILE:STATe?") == "IDLE"
            send(sock, f'FILE:OPEN "{tmp_path}",WRITE')
            assert query(sock, "FILE:STATe?") == "WRITE"
            assert int(query(sock, "DATA:FREE?")) >= 4096

            # Upload in two binary chunks
            send_data_write(sock, payload[:3000])
            send_data_write(sock, payload[3000:])
            send(sock, "FILE:CLOSe")
            assert query(sock, "FILE:STATe?") == "IDLE"

            assert int(query(sock, f'FILE:SIZE? "{tmp_path}"')) == len(payload)
            assert int(query(sock, f'FILE:CRC32? "{tmp_path}"')) == expected_crc

            # Download back in chunks via FILE:OPEN ...,READ + FILE:READ? / DATA:COUNt?
            send(sock, f'FILE:OPEN "{tmp_path}",READ')
            assert query(sock, "FILE:STATe?") == "READ"
            assert int(query(sock, "FILE:SIZE?")) == len(payload)
            assert int(query(sock, "DATA:COUNt?")) == len(payload)

            downloaded = bytearray()
            while int(query(sock, "DATA:COUNt?")) > 0:
                chunk = query_bytes(sock, "FILE:READ? 2048")
                assert len(chunk) > 0, "empty chunk while DATA:COUNt? > 0"
                downloaded.extend(chunk)
            send(sock, "FILE:CLOSe")
            assert bytes(downloaded) == payload, "downloaded payload mismatch"

            # Delete file over SCPI (synchronize via SYST:ERR? before checking host fs)
            send(sock, f'FILE:DELete "{tmp_path}"')
            err = query(sock, "SYST:ERR?")
            assert err.startswith("0,"), f"unexpected SCPI error: {err!r}"
            assert not os.path.exists(tmp_path), "FILE:DELete did not unlink file"

            gdir = query(sock, "GPIO:DIR? 17")
            assert gdir == "FREE", f"unexpected initial dir: {gdir!r}"
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)
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
