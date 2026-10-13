#!/usr/bin/env python3
"""Scan, join and leave Wi-Fi on the esp32s3-ble-wifi firmware, over BLE.

BLE stays the control link throughout: commands are ordinary SCPI written to
the RX characteristic, replies arrive as TX notifications (see
glue/usbscpi_nimble.h for the wire contract). Requires `pip install bleak`.

  ble_wifi.py devices
  ble_wifi.py -d <address|name> info
  ble_wifi.py -d <address|name> scan
  ble_wifi.py -d <address|name> connect --ssid "MyNetwork"   # prompts for the password
  ble_wifi.py -d <address|name> status
  ble_wifi.py -d <address|name> disconnect
  ble_wifi.py -d <address|name> send '*IDN?'

The first connection pairs (LE Secure Connections, Just Works) and bonds.
"""
import argparse
import asyncio
import getpass
import re
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

SERVICE = "7d7e0001-ce83-4a46-ac8a-2e014163ef3e"
RX = "7d7e0002-ce83-4a46-ac8a-2e014163ef3e"
TX = "7d7e0003-ce83-4a46-ac8a-2e014163ef3e"

POLL_S = 0.25


class DeviceError(Exception):
    """The device queued an error, as SYSTem:ERRor? reports it."""


ERROR_ENTRY = re.compile(r'[+-]?\d+,"')
NO_ERROR = re.compile(r"\+?0,")


def take_reply(buf):
    """Split one reply off the front of `buf`: (text, is_block, rest), or
    (None, False, buf) while it is incomplete. A reply is a line, or a
    definite-length block `#<n><len><payload>` followed by its newline."""
    buf = buf.lstrip(b"\r\n")
    if buf[:1] == b"#":
        if len(buf) < 2:
            return None, False, buf
        n = int(buf[1:2])
        if len(buf) < 2 + n:
            return None, False, buf
        end = 2 + n + int(buf[2:2 + n])
        if len(buf) < end:
            return None, False, buf
        return buf[2 + n:end].decode("utf-8", "replace"), True, buf[end:]
    line, sep, rest = buf.partition(b"\n")
    if not sep:
        return None, False, buf
    return line.rstrip(b"\r").decode("utf-8", "replace"), False, rest


def block(text):
    """A string parameter as a definite-length block: no quoting to get wrong,
    and UTF-8 passes through untouched."""
    data = text.encode()
    if b"\n" in data or b"\r" in data:
        raise ValueError("line breaks are not allowed")
    return b"#%d%d%s" % (len(str(len(data))), len(data), data)


def is_error_query(cmd):
    return re.fullmatch(r":?SYST(EM)?:ERR(OR)?\?", cmd.strip().upper()) is not None


class Link:
    def __init__(self, device, timeout):
        self.device = device
        self.timeout = timeout
        self.buf = b""
        self.ready = asyncio.Event()

    async def __aenter__(self):
        want = self.device.upper()
        dev = await BleakScanner.find_device_by_filter(
            lambda d, _adv: want in (d.address.upper(), (d.name or "").upper()),
            timeout=10.0)
        if dev is None:
            raise SystemExit(f"{self.device}: not found (try `devices`)")
        self.client = BleakClient(dev)
        await self.client.connect()
        try:
            await self.client.pair()
        except NotImplementedError:
            pass  # macOS pairs on its own when the encrypted RX is first written
        except BleakError as e:
            # The board asks for security as soon as it connects, so the OS may
            # already be pairing; the encrypted RX write fails if it did not.
            print(f"pair: {e}", file=sys.stderr)
        await self.client.start_notify(TX, self._on_notify)
        return self

    async def __aexit__(self, *exc):
        await self.client.disconnect()

    def _on_notify(self, _char, data):
        self.buf += bytes(data)
        self.ready.set()

    async def write(self, cmd):
        data = (cmd if isinstance(cmd, bytes) else cmd.encode()) + b"\n"
        step = max(20, self.client.mtu_size - 3)
        for i in range(0, len(data), step):
            await self.client.write_gatt_char(RX, data[i:i + step], response=True)

    async def read(self):
        """The next reply as (text, is_block)."""
        async def wait():
            while True:
                text, is_block, self.buf = take_reply(self.buf)
                if text is not None:
                    return text, is_block
                self.ready.clear()
                await self.ready.wait()
        return await asyncio.wait_for(wait(), self.timeout)

    async def send(self, cmd):
        """Run one command or query and return its output, or None.

        Sends `*CLS;<cmd>;:SYSTem:ERRor?`, as host/rust's send_checked does: a
        failed query sends nothing, so the trailing error query is what always
        answers. A device error raises DeviceError with the whole queue."""
        cmd = cmd if isinstance(cmd, bytes) else cmd.encode()
        await self.write(b"*CLS;" + cmd + b";:SYSTem:ERRor?")
        output = None
        while True:
            text, is_block = await self.read()
            if not is_block and ERROR_ENTRY.match(text):
                break
            output = text
        errors = []
        while not NO_ERROR.match(text):
            errors.append(text)
            await self.write("SYSTem:ERRor?")
            text, _ = await self.read()
        if errors:
            raise DeviceError("; ".join(errors))
        return output

    async def until(self, query, done, limit_s):
        """Poll a job's STATe? until it reaches `done`. FAILED queues its reason,
        which send() raises."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + limit_s
        while True:
            state = await self.send(query)
            if state in done:
                return state
            if state == "FAILED":
                raise DeviceError(f"{query} FAILED")
            if loop.time() > deadline:
                raise DeviceError(f"{query} still {state} after {limit_s}s")
            await asyncio.sleep(POLL_S)


async def devices(_args):
    found = await BleakScanner.discover(timeout=5.0, service_uuids=[SERVICE], return_adv=True)
    for dev, adv in found.values():
        print(f"{dev.address}  {adv.rssi:4d} dBm  {dev.name or ''}")
    if not found:
        print("no devices found", file=sys.stderr)


async def info(link, _args):
    print(await link.send("*IDN?"))
    desc = await link.send("SYST:HELP:DESC?")
    for line in desc.splitlines():
        if line.startswith(("CMD ", "WF ")):
            print(" ", line.split(" ", 2)[0], line.split(" ", 2)[1])


async def scan(link, _args):
    await link.send("WLAN:SCAN:STARt")
    await link.until("WLAN:SCAN:STATe?", ("DONE",), 15)
    print("ssid,rssi,channel,authmode,bssid")
    for i in range(int(await link.send("WLAN:SCAN:COUNt?"))):
        print(await link.send(f"WLAN:SCAN:FETCh? {i}"))


async def show_result(link):
    if int(await link.send("WLAN:CONNect:COUNt?")):
        result, ip, reason = (await link.send("WLAN:CONNect:FETCh? 0")).split(",")
        print(f"{result} ip={ip.strip(chr(34)) or '-'} reason={reason}")


async def connect(link, args):
    password = args.password if args.password is not None else getpass.getpass("Password: ")
    await link.send(b"WLAN:CONNect:STARt " + block(args.ssid) + b"," + block(password))
    try:
        await link.until("WLAN:CONNect:STATe?", ("DONE",), 35)
    finally:
        await show_result(link)


async def status(link, _args):
    print(await link.send("WLAN:CONNect:STATe?"))
    await show_result(link)


async def disconnect(link, _args):
    await link.send("WLAN:CONNect:STOP")
    print(await link.until("WLAN:CONNect:STATe?", ("IDLE",), 10))
    await show_result(link)


async def send(link, args):
    if is_error_query(args.cmd):
        await link.write(args.cmd)
        print((await link.read())[0])
        return
    output = await link.send(args.cmd)
    if output is not None:
        print(output)


async def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("-d", "--device", help="BLE address or name, from `devices`")
    p.add_argument("--timeout", type=float, default=10.0, help="seconds to wait for a reply")
    sub = p.add_subparsers(dest="action", required=True)
    sub.add_parser("devices", help="list nearby boards")
    sub.add_parser("info", help="*IDN? and the described commands and workflows")
    sub.add_parser("scan", help="scan for access points")
    c = sub.add_parser("connect", help="join a network")
    c.add_argument("--ssid", required=True)
    c.add_argument("--password", help="omit to be prompted")
    sub.add_parser("status", help="association state and result")
    sub.add_parser("disconnect", help="leave the network")
    s = sub.add_parser("send", help="one raw SCPI command or query")
    s.add_argument("cmd")
    args = p.parse_args()

    if args.action == "devices":
        return await devices(args)
    if not args.device:
        p.error("--device is required")
    action = {"info": info, "scan": scan, "connect": connect, "status": status,
              "disconnect": disconnect, "send": send}[args.action]
    async with Link(args.device, args.timeout) as link:
        await action(link, args)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except DeviceError as e:
        sys.exit(f"device error: {e}")
    except asyncio.TimeoutError:
        sys.exit("timed out waiting for a reply")
