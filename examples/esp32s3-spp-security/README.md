# ESP32-S3 BLE SPP Security over USBTMC/SCPI

This example turns an ESP32-S3 into a **BLE SPP peripheral** whose pairing and
connection security are driven and inspected from a host over **USBTMC/SCPI**.

It adapts Espressif's NimBLE `ble_spp/spp_server` example (the SPP GATT service,
advertising, and GAP handling) and exposes it through `iotsploit-usb`'s
USBTMC/SCPI control path — the same TinyUSB + libSCPI glue used by the other
examples in this repo. The SPP data path is intentionally minimal; the point of
the example is to **connect, pair, and report the negotiated BLE security
level**.

## What it does

- Enumerates as a USBTMC instrument (`0x1209:0x0001`).
- Runs the NimBLE host stack as an SPP peripheral (service UUID `0xABF0`).
- Security Manager configured for LE Secure Connections + MITM + bonding, with
  `KeyboardDisplay` IO capability (the host acts as the keyboard/display via
  SCPI, so passkey / numeric-comparison pairing is fully supported).
- Reports the connection security state as an LE security level (1–4).

## Hardware

- ESP32-S3 dev board.
- **USB-OTG** port (native USB) → host, enumerates as USBTMC.
- **CP210x UART** → host, carries the `esp_log` console output (`idf.py monitor`).

## SCPI interface

| Command | Type | Description |
|---|---|---|
| `*IDN?` | query | `IoTSploit,ESP32S3-SPP-SEC,0001,0.1.0` |
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

d = pyvisa.ResourceManager().open_resource("USB0::0x1209::0x0001::0001::INSTR")
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
