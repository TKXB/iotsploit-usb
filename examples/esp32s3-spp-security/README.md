# ESP32-S3 BLE SPP Security over USBTMC/SCPI

This example turns an ESP32-S3 into a **BLE SPP peripheral** whose pairing and
connection security are driven and inspected from a host over **USBTMC/SCPI**.

It adapts Espressif's NimBLE `ble_spp/spp_server` example (the SPP GATT service,
advertising, and GAP handling) and exposes it through `iotsploit-usb`'s
USBTMC/SCPI control path — the same TinyUSB + libSCPI glue used by the other
examples in this repo. The SPP characteristic also carries SCPI as a **third
transport**, so the full command surface is reachable over BLE as well as over
USBTMC and (in the esp32s3 example) TCP.

## What it does

- Enumerates as a USBTMC instrument (`0x1209:0x0001`).
- Runs the NimBLE host stack as an SPP peripheral (service UUID `0xABF0`).
- Carries SCPI over the SPP characteristic (`0xABF1`): a central subscribes to
  notifications, writes command bytes, and reads the notified replies.
- Security Manager configured for LE Secure Connections + MITM + bonding, with
  `KeyboardDisplay` IO capability (the host acts as the keyboard/display via
  SCPI, so passkey / numeric-comparison pairing is fully supported).
- Reports the connection security state as an LE security level (1–4).

## SCPI over BLE

The SPP characteristic `0xABF1` is a Nordic-UART-style byte pipe wired into a
second `usbscpi_t` context, exactly as `net_scpi.c` wires SCPI onto a TCP socket
in the esp32s3 example. A central:

1. connects and subscribes to notifications on `0xABF1` (writes its CCCD),
2. writes a SCPI command line (ending in `\n`) to `0xABF1`, and
3. receives the reply as one or more notifications, reassembled by the `\n`
   terminator — the reply is chunked to the negotiated ATT MTU.

It is a distinct context from the USB one: a `usbscpi_t` holds exactly one
response callback, so a command arriving over BLE on the USB context would route
its reply out over USB. The BLE context has its own line buffer and error queue,
and is cleared on disconnect so a central that drops mid-block cannot poison the
next session. Both share the same command table and descriptor, so `*IDN?`,
`SYSTem:HELP:DESCription?`, and every `BLE:*` command behave identically on
either transport.

```python
# Example with the `bleak` BLE client (central on the host).
import asyncio
from bleak import BleakClient, BleakScanner

SPP_CHR = "0000abf1-0000-1000-8000-00805f9b34fb"

async def main():
    dev = await BleakScanner.find_device_by_name("iotsploit-spp-sec")
    async with BleakClient(dev) as c:
        reply = bytearray()
        done = asyncio.Event()

        def on_notify(_handle, data):
            reply.extend(data)
            if reply.endswith(b"\n"):
                done.set()

        await c.start_notify(SPP_CHR, on_notify)
        await c.write_gatt_char(SPP_CHR, b"*IDN?\n", response=False)
        await asyncio.wait_for(done.wait(), timeout=5)
        print(reply.decode().strip())   # IoTSploit,ESP32S3-SPP-SEC,<MAC>,0.1.0

asyncio.run(main())
```

> The BLE SCPI pipe is not encryption-gated in this example: a central can send
> commands as soon as it subscribes, before pairing. Drive `BLE:PERiph` /
> `BLE:PAIR` (over USB or BLE) to negotiate a secure link, then read `BLE:SEC?`.

## Hardware

- ESP32-S3 dev board.
- **USB-OTG** port (native USB) → host, enumerates as USBTMC.
- **CP210x UART** → host, carries the `esp_log` console output (`idf.py monitor`).

## SCPI interface

| Command | Type | Description |
|---|---|---|
| `*IDN?` | query | `IoTSploit,ESP32S3-SPP-SEC,<factory MAC>,0.1.0` |
| `SYSTem:HELP:DESCription?` | query | Machine-readable command/workflow metadata |
| `BLE:ADV:STARt` | command | Start BLE SPP advertising |
| `BLE:ADV:STOP` | command | Stop advertising |
| `BLE:CONNect:STATe?` | query | `0` idle, `1` connected |
| `BLE:CONNect:STATus?` | query | Last GAP status/reason code |
| `BLE:PAIR` | command | Initiate pairing/security on the active connection |
| `BLE:PAIR:STATe?` | query | `0` idle, `1` in-progress, `2` passkey-needed, `3` numcmp-needed, `4` done, `5` failed, `6` display-key |
| `BLE:PAIR:PASSKey <n>` | command | Enter the 6-digit passkey shown on the peer |
| `BLE:PAIR:PASSKey?` | query | Get the passkey to enter on the peer |
| `BLE:PAIR:NUMCmp?` | query | Get the numeric comparison value |
| `BLE:PAIR:CONFirm <0\|1>` | command | Reject / confirm numeric comparison |
| `BLE:SEC?` | query | `<mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>` |

### Security level mapping

| Level | Meaning |
|---|---|
| `1` | Not encrypted |
| `2` | Encrypted, unauthenticated (typically Just Works) |
| `3` | Encrypted and authenticated |
| `4` | Encrypted, authenticated, 16-byte key (LE Secure Connections proxy) |

> `key_size == 16` is used as a practical proxy for L4. A legacy pairing that
> also negotiates a 16-byte key would be reported as L4.

## Build & flash

```sh
. /home/tkxb/HDD/Projects/esp-idf/export.sh
cd third_party/iotsploit-usb/examples/esp32s3-spp-security
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor    # CP210x UART port for the console
```

## Host usage (pyvisa)

```python
import time, pyvisa

rm = pyvisa.ResourceManager()
# The serial number is the board's factory MAC; list_resources() shows it.
d = rm.open_resource(next(r for r in rm.list_resources() if '0x1209::0x0001' in r))
print(d.query("*IDN?"))
print(d.query("SYSTem:HELP:DESCription?"))

d.write("BLE:ADV:STARt")

# Connect from a phone / BLE central, then:
while int(d.query("BLE:CONNect:STATe?")) == 0:
    time.sleep(0.5)

d.write("BLE:PAIR")
# If the central requires passkey / numeric comparison, drive it:
#   state 2 -> d.write("BLE:PAIR:PASSKey <n>")
#   state 3 -> print(d.query("BLE:PAIR:NUMCmp?")); d.write("BLE:PAIR:CONFirm 1")
#   state 6 -> print(d.query("BLE:PAIR:PASSKey?"))  # enter it on the peer
while int(d.query("BLE:PAIR:STATe?")) not in (4, 5):
    time.sleep(0.5)

print(d.query("BLE:SEC?"))   # e.g. D8:3A:DD:E4:7A:98,4,1,1,1,16
```
