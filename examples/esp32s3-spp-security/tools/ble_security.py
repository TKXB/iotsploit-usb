#!/usr/bin/env python3
"""Drive the esp32s3-spp-security firmware over SCPI and print the BLE security level.

Two transports are supported:
  * raw USBTMC kernel device  (Linux: /dev/usbtmcN)   -- default, no deps
  * pyvisa                    (cross-platform)         -- pass --visa

Typical flow:
  1. Start advertising.
  2. Connect + pair from a phone / BLE central.
  3. If the central asks for a passkey or numeric comparison, this script drives it.
  4. Query BLE:SEC? and print the decoded security level.

Examples:
  ./ble_security.py                       # /dev/usbtmc0, full flow
  ./ble_security.py --dev /dev/usbtmc3    # pick the USBTMC node
  ./ble_security.py --visa                # use pyvisa (USB0::0x1209::0x0001::0001::INSTR)
  ./ble_security.py --sec-only            # just read BLE:SEC? once and exit
"""
import argparse
import os
import sys
import time

LEVELS = {
    1: "L1  not encrypted",
    2: "L2  encrypted, unauthenticated (Just Works)",
    3: "L3  encrypted + authenticated",
    4: "L4  encrypted + authenticated + 16-byte key (LE Secure Connections)",
}

# BLE:PAIR:STATe? values (see main/ble_spp_security.h)
PAIR_IDLE, PAIR_PROGRESS, PAIR_PASSKEY, PAIR_NUMCMP, PAIR_DONE, PAIR_FAILED, PAIR_DISPLAY = range(7)


class UsbtmcLink:
    """Minimal raw-USBTMC transport: write a command, read the reply."""
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR)

    def write(self, cmd):
        os.write(self.fd, cmd.encode())

    def query(self, cmd, wait=0.25):
        os.write(self.fd, cmd.encode())
        time.sleep(wait)
        return os.read(self.fd, 4096).decode("utf-8", "replace").strip()

    def close(self):
        os.close(self.fd)


class VisaLink:
    """pyvisa transport (query() appends the read for you)."""
    def __init__(self, resource):
        import pyvisa
        self.inst = pyvisa.ResourceManager().open_resource(resource)

    def write(self, cmd):
        self.inst.write(cmd)

    def query(self, cmd, wait=0.0):
        return self.inst.query(cmd).strip()

    def close(self):
        self.inst.close()


def decode_sec(row):
    """Turn 'mac,level,enc,auth,bonded,ks' into a human line."""
    try:
        mac, level, enc, auth, bonded, ks = row.split(",")
        level = int(level)
    except ValueError:
        return f"(no active connection or bad reply: {row!r})"
    return (f"peer={mac}  {LEVELS.get(level, f'L{level} ?')}\n"
            f"    encrypted={enc} authenticated={auth} bonded={bonded} key_size={ks}")


def run_pairing(link, timeout=45.0):
    """Kick off BLE:PAIR and service any interactive prompts until done/failed."""
    print("[*] BLE:PAIR")
    link.write("BLE:PAIR")
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        st = int(link.query("BLE:PAIR:STATe?") or "0")
        if st != last:
            print(f"    pair state = {st}")
            last = st
        if st == PAIR_DONE:
            print("[+] pairing complete")
            return True
        if st == PAIR_FAILED:
            print("[!] pairing failed")
            return False
        if st == PAIR_PASSKEY:
            pk = input("    >> enter the 6-digit passkey shown on the peer: ").strip()
            link.write(f"BLE:PAIR:PASSKey {pk}")
        elif st == PAIR_NUMCMP:
            num = link.query("BLE:PAIR:NUMCmp?")
            ok = input(f"    >> peer shows {num} — does it match? [Y/n]: ").strip().lower()
            link.write("BLE:PAIR:CONFirm " + ("0" if ok == "n" else "1"))
        elif st == PAIR_DISPLAY:
            print(f"    >> enter this passkey on the peer: {link.query('BLE:PAIR:PASSKey?')}")
        time.sleep(0.4)
    print("[!] pairing timed out")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dev", default="/dev/usbtmc0", help="USBTMC device node")
    ap.add_argument("--visa", action="store_true", help="use pyvisa instead of raw USBTMC")
    ap.add_argument("--resource", default="USB0::0x1209::0x0001::0001::INSTR",
                    help="pyvisa resource string")
    ap.add_argument("--sec-only", action="store_true",
                    help="just read BLE:SEC? once and exit (no adv/pair)")
    ap.add_argument("--no-pair", action="store_true",
                    help="advertise + wait for connect, but do not initiate pairing")
    args = ap.parse_args()

    link = VisaLink(args.resource) if args.visa else UsbtmcLink(args.dev)
    try:
        print("[*] *IDN? ->", link.query("*IDN?"))

        if args.sec_only:
            print(decode_sec(link.query("BLE:SEC?")))
            return

        print("[*] BLE:ADV:STARt")
        link.write("BLE:ADV:STARt")

        print("[*] waiting for a central to connect ...")
        while int(link.query("BLE:CONNect:STATe?") or "0") == 0:
            time.sleep(0.5)
        print("[+] connected")

        if not args.no_pair:
            run_pairing(link)

        print("[*] BLE:SEC? ->")
        print("   ", decode_sec(link.query("BLE:SEC?")))
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
