#!/usr/bin/env python3
"""Host-side smoke test for the ESP32-S3 USBTMC SCPI demo.

Exercises the Wi-Fi and BLE scan commands over the Linux kernel USBTMC driver
(``/dev/usbtmcN``). The driver handles USBTMC framing, so we just write SCPI
lines and read the responses.

Usage:
    sudo python3 scan_test.py                 # auto-detect /dev/usbtmc*
    sudo python3 scan_test.py -d /dev/usbtmc0 # explicit device node
    sudo python3 scan_test.py --ble-secs 8    # longer BLE discovery

Root is required because the usbtmc character device is root-only by default.
A pyvisa equivalent is shown in the example README.
"""
import argparse
import glob
import os
import sys
import time


class UsbTmc:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)

    def cmd(self, text, read=True, rdlen=4096):
        os.write(self.fd, (text + "\n").encode())
        if not read:
            return None
        return os.read(self.fd, rdlen).decode(errors="replace").strip()

    def close(self):
        os.close(self.fd)


def wait_done(dev, query, timeout_s, poll_s):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if dev.cmd(query) == "1":
            return True
        time.sleep(poll_s)
    return False


def scan_wifi(dev):
    print("\n--- Wi-Fi SSID scan ---")
    dev.cmd("WLAN:SCAN", read=False)
    if not wait_done(dev, "WLAN:SCAN:DONE?", timeout_s=15, poll_s=0.25):
        print("  timed out waiting for scan to finish")
        return
    n = int(dev.cmd("WLAN:SCAN:COUNt?"))
    print(f"  {n} AP(s) found  (\"ssid\",rssi,channel,authmode,bssid)")
    for i in range(n):
        print(f"  [{i:2}] {dev.cmd(f'WLAN:SCAN? {i}')}")


def scan_ble(dev, secs):
    print(f"\n--- BLE scan ({secs}s) ---")
    dev.cmd(f"BLE:SCAN {secs}", read=False)
    if not wait_done(dev, "BLE:SCAN:DONE?", timeout_s=secs + 10, poll_s=0.5):
        print("  timed out waiting for discovery to finish")
        return
    n = int(dev.cmd("BLE:SCAN:COUNt?"))
    print(f"  {n} device(s) found  (addr,rssi,\"name\",adv_type)")
    for i in range(n):
        print(f"  [{i:2}] {dev.cmd(f'BLE:SCAN? {i}')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-d", "--device", help="usbtmc node (default: first /dev/usbtmc*)")
    ap.add_argument("--ble-secs", type=int, default=5, help="BLE discovery duration")
    ap.add_argument("--wifi-only", action="store_true")
    ap.add_argument("--ble-only", action="store_true")
    args = ap.parse_args()

    path = args.device
    if not path:
        nodes = sorted(glob.glob("/dev/usbtmc*"))
        if not nodes:
            sys.exit("no /dev/usbtmc* device found - is the board enumerated?")
        path = nodes[0]

    try:
        dev = UsbTmc(path)
    except PermissionError:
        sys.exit(f"permission denied opening {path} (run with sudo)")
    except FileNotFoundError:
        sys.exit(f"{path} not found")

    print(f"device: {path}")
    print("*IDN?     ->", dev.cmd("*IDN?"))
    print("SYST:CAP? ->", dev.cmd("SYST:CAP?"))

    if not args.ble_only:
        scan_wifi(dev)
    if not args.wifi_only:
        scan_ble(dev, args.ble_secs)

    print("\nSYST:ERR? ->", dev.cmd("SYST:ERR?"))
    dev.close()


if __name__ == "__main__":
    main()
