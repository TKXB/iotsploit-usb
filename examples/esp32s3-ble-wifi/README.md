# ESP32-S3 Wi-Fi over BLE SCPI

The host connects over BLE, sends ordinary SCPI, and uses it to scan for, join
and leave Wi-Fi networks. BLE stays the control link throughout; Wi-Fi never
carries SCPI in this example.

```text
Python host ──SCPI over GATT──▶ NimBLE glue ──▶ SCPI core ──▶ WLAN jobs ──▶ ESP-IDF Wi-Fi
(tools/ble_wifi.py)   (glue/usbscpi_nimble.c)  (src/usbscpi.c)  (examples/components/wlan)
```

- The WLAN jobs and the Wi-Fi owner are the ones `examples/esp32s3` uses,
  shared through `examples/components/wlan/`. Over BLE every WLAN job is
  allowed: the link does not ride on Wi-Fi, and coexistence keeps it up through
  a scan or an association.
- Credentials stay in RAM, exactly as in `examples/esp32s3`. A successful
  credential is reused for one bounded reconnect; nothing joins at boot.
- The BLE wire contract (service and characteristic UUIDs, framing, security)
  is documented in `glue/usbscpi_nimble.h`.

## SCPI interface

| Purpose | Commands |
|---|---|
| Identity and discovery | `*IDN?`, `SYSTem:HELP:DESCription?` |
| Scan | `WLAN:SCAN:STARt`, `WLAN:SCAN:STATe?`, `WLAN:SCAN:COUNt?`, `WLAN:SCAN:FETCh? <index>` |
| Join | `WLAN:CONNect:STARt <ssid>,<password>` (quoted strings or definite-length blocks) |
| Progress and result | `WLAN:CONNect:STATe?`, `WLAN:CONNect:COUNt?`, `WLAN:CONNect:FETCh? 0` |
| Leave, or forget an offline result | `WLAN:CONNect:STOP`, `WLAN:CONNect:CLEar` |
| Errors | `SYSTem:ERRor?` |

`*IDN?` is `IoTSploit,ESP32S3-BLE-WIFI,<factory MAC>,<version>`. The descriptor
also lists the `wifi-scan` and `wifi-connect` workflows.

## Security

The device pairs with LE Secure Connections, Just Works, and bonds. RX writes
require encryption, and the glue serves a link only once it is encrypted with
an LE Secure Connections bond; anything else is disconnected and its bond
deleted.

Just Works protects the Wi-Fi password from passive eavesdropping. It does not
stop an active man in the middle, and any central in range can pair while the
board advertises. Bonds persist in NVS; erase them with `idf.py erase-flash`.

## Build and flash

```sh
. $IDF_PATH/export.sh
cd examples/esp32s3-ble-wifi
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # UART console
```

The board advertises as `iotsploit-wifi` from boot and again after every
disconnect.

## Host

```sh
pip install bleak
python tools/ble_wifi.py devices
python tools/ble_wifi.py -d <address|name> info
python tools/ble_wifi.py -d <address|name> scan
python tools/ble_wifi.py -d <address|name> connect --ssid "MyNetwork"   # prompts for the password
python tools/ble_wifi.py -d <address|name> status
python tools/ble_wifi.py -d <address|name> disconnect
python tools/ble_wifi.py -d <address|name> send '*IDN?'
```

The client pairs on first use (`pair()` on Linux and Windows; macOS pairs when
the encrypted RX characteristic is first written). Each command is sent as
`*CLS;<cmd>;:SYSTem:ERRor?`, like the Rust host's checked send, so a failed
query reports its error at once instead of timing out. Credentials go as
definite-length blocks, so quotes, commas, semicolons and UTF-8 need no
escaping.

## Hardware checks

Host checks cannot prove these; run them on a board:

- Unencrypted access is refused, and a legacy (non-Secure-Connections) pairing
  is disconnected.
- `info` reads the whole descriptor at the default and at a negotiated MTU.
- `scan`, `connect`, `status` and `disconnect` complete, and the BLE link
  survives the full scan and the association.
- A dropped link reconnects and the next session starts clean.
