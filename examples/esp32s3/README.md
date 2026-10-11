# ESP32-S3 USBTMC Demo

ESP32-S3 USBTMC (Test and Measurement Class) demo using TinyUSB.

## Build

Tested with ESP-IDF v5.2.2. Load the ESP-IDF environment first (adjust the path
to your installation):

```bash
. $HOME/esp/esp-idf/export.sh
cd examples/esp32s3
idf.py set-target esp32s3
idf.py build
```

The build produces `build/esp32s3_usbscpi_demo.bin`.

## Flash & Monitor

Find the board's serial port, then flash and watch it boot:

```bash
python3 -m serial.tools.list_ports -v
idf.py -p <port> flash monitor
```

`idf.py flash` toggles RTS/DTR to enter download mode, which works through the
common CP210x USB-UART bridges. The boot log ends with `Calling app_main()`.

If the monitor warns `Detected size(8192k) larger than the size in the binary
image header(2048k)`, the board has 8 MB of flash while the image is configured
for 2 MB. It still boots; set the flash size in `idf.py menuconfig` to use the
rest.

Once the USB port enumerates, check it from the host:

```bash
iotsploit-host devices
iotsploit-host info
```

## Host Test (pyvisa)

```python
import pyvisa
rm = pyvisa.ResourceManager()
# The serial number is the board's factory MAC; list_resources() shows it.
d = rm.open_resource(next(r for r in rm.list_resources() if '0x1209::0x0001' in r))
print(d.query('*IDN?'))
print(d.query('SYST:CAP?'))
d.write('GPIO:SET 2,1')
print(d.query('GPIO:GET? 2'))
print(d.query('ADC:READ? 0'))
print(d.query_binary_values('DATA:READ? 64', datatype='B'))
```

`ADC? <channel>` reads any ADC1 channel, 0 to 9; on the ESP32-S3, channel `n`
is GPIO `n+1`. A channel is configured (12 dB attenuation, about 0 to 3.1 V) the
first time it is read. Any other channel number is refused with
`-222,"Data out of range"`. The reply is the raw count.

## Wi-Fi connection (USB provisioning)

The Radio Bench's **Wi-Fi → Connect to AP** view imports a UTF-8 `.txt`
password list or accepts pasted passwords, one per line. The UI retains the
list and sends one candidate at a time. Order, duplicates and password spaces
are preserved; empty lines are ignored. Thousands of candidates do not consume
firmware list storage. Keep the UI open and USB attached while the list runs.

Each credential is an asynchronous job:

```text
WLAN:CONNect:STARt "Lab-Router","candidate-password"
WLAN:CONNect:STATe?             -> RUNNING, then DONE or FAILED
WLAN:CONNect:COUNt?             -> 1 after completion
WLAN:CONNect:FETCh? 0           -> CONNECTED,"192.168.1.57",0
WLAN:CONNect:STOP               # cancel or disconnect; poll STATE? for IDLE
WLAN:CONNect:CLEar              # clear an offline result
```

`STARt` accepts an SSID of 1–32 UTF-8 bytes and a password of 0–63 bytes.
Double quotes inside either value must be doubled; for example `"a""b"`
represents `a"b`. Empty password supports an open AP through SCPI; the list UI
ignores empty lines. Enterprise Wi-Fi credentials and 64-character raw PSKs
are outside this interface.

Association and DHCP each have a 15-second deadline. Results contain
`result,ip,reason`: `AUTH_FAILED`, `TIMEOUT`, `AP_NOT_FOUND`, `DHCP_TIMEOUT`,
`DISCONNECTED`, `ERROR`, `STOPPED`, or `CONNECTED`. `reason` preserves the
ESP-IDF disconnect reason (or API error). An authentication/handshake failure
is an attempt result, not proof that the password is incorrect. A failed job
queues the standard execution error once when `STATe?` is queried.

The UI advances on `AUTH_FAILED` and `TIMEOUT`; other failures pause the run.
Stop and closing the board session wait for cancellation before releasing USB.
Cancellation waits for `WIFI_EVENT_STA_STOP` and restarts the station driver,
so stopping before association also completes without leaking old events.
On a new run, the UI cancels any attempt left by an earlier session before
sending its first candidate. It does not automatically resume after USB loss.
After success the board keeps the connection and reuses the successful
credential for one bounded reconnect if the link drops.

Credentials use Wi-Fi RAM storage and are not persisted to NVS. There are no
build-time credentials or automatic boot association. TCP SCPI (5025) and the
BLE stream listener (5026) start after USB provisioning obtains an IPv4 lease.
Mutating Wi-Fi connection commands are USB-only; queries also work over TCP.
Scanning and association attempts are serialized by the Wi-Fi owner.

To bundle a built image for the UI's Firmware Manager:

```bash
# From the ui repository root:
python3 tools/release/bundle-esp32s3.py third_party/iotsploit-usb/examples/esp32s3/build
python3 tools/release/bundle-esp32s3.py third_party/iotsploit-usb/examples/esp32s3/build --check
```

## Wi-Fi / BLE Scan (USBTMC SCPI)

Both scanners use the same async pattern: trigger, poll `:DONE?`, then fetch
rows one at a time so a single response never overflows the 512 B IN buffer.

```python
import time

# --- Wi-Fi SSID scan ---
d.write('WLAN:SCAN')                         # non-blocking trigger
while int(d.query('WLAN:SCAN:DONE?')) == 0:
    time.sleep(0.2)
for i in range(int(d.query('WLAN:SCAN:COUNT?'))):
    # "<ssid>",<rssi>,<channel>,<authmode>,<bssid>
    print(d.query(f'WLAN:SCAN? {i}'))

# --- BLE device scan (default 5 s; optional duration arg) ---
d.write('BLE:SCAN 5')
while int(d.query('BLE:SCAN:DONE?')) == 0:
    time.sleep(0.2)
for i in range(int(d.query('BLE:SCAN:COUNT?'))):
    # <addr>,<rssi>,"<name>",<adv_type>
    print(d.query(f'BLE:SCAN? {i}'))
```

### Command reference

| Command | Returns | Notes |
|---|---|---|
| `WLAN:SCAN` | — | Start non-blocking all-channel scan |
| `WLAN:SCAN:DONE?` | `0`/`1` | `1` once results are ready |
| `WLAN:SCAN:COUNT?` | uint | AP count (max 20) |
| `WLAN:SCAN? <i>` | CSV | `"<ssid>",<rssi>,<ch>,<authmode>,<bssid>` |
| `BLE:SCAN [secs]` | — | Start GAP discovery, default 5 s |
| `BLE:SCAN:DONE?` | `0`/`1` | `1` once discovery completed |
| `BLE:SCAN:COUNT?` | uint | distinct devices (max 20) |
| `BLE:SCAN? <i>` | CSV | `<addr>,<rssi>,"<name>",<adv_type>` |

> Wi-Fi and BLE share the radio; scan one at a time. The `*:SCAN` triggers
> return immediately, so `tud_task()` is never blocked during a scan.

## BLE Connect + Pair (USBTMC SCPI)

After a scan, connect to a device by its scan index, optionally pair (the SMP
is configured as IO-capability **KeyboardDisplay**, MITM, LE Secure
Connections), then read back the negotiated security parameters. The PC is the
keyboard/display: when the peer asks for a PIN, you inject the passkey over
SCPI.

```python
import time

# Assumes a prior BLE:SCAN; connect to device #0
d.write('BLE:CONNect 0')                 # 0=idle 1=connecting 2=connected 3=failed
while int(d.query('BLE:CONNect:STATe?')) == 1:
    time.sleep(0.2)

d.write('BLE:PAIR')                       # initiate pairing/encryption
while True:
    ps = int(d.query('BLE:PAIR:STATe?'))  # see table below
    if ps == 2:                           # peer shows a passkey -> type it in
        d.write(f'BLE:PAIR:PASSKey {input("passkey: ")}')
    elif ps == 6:                         # we show a passkey  -> enter on peer
        print('enter on peer:', d.query('BLE:PAIR:PASSKey?'))
    elif ps == 3:                         # numeric comparison -> confirm match
        print('compare:', d.query('BLE:PAIR:NUMCmp?'))
        d.write('BLE:PAIR:CONFirm 1')
    elif ps in (4, 5):
        break
    time.sleep(0.3)

# <mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>
print(d.query('BLE:SEC?'))
```

The Rust host wraps this: `sudo iotsploit-host workflow ble-connect-pair`.

### Connect / pair command reference

| Command | Returns | Notes |
|---|---|---|
| `BLE:CONNect <i>` | — | Connect to scan result `i` (reuses its address + type) |
| `BLE:CONNect:STATe?` | int | `0` idle, `1` connecting, `2` connected, `3` failed |
| `BLE:CONNect:STATus?` | int | Last GAP status/reason code (diagnostics; `0` = ok) |
| `BLE:DISConnect` | — | Drop the current connection |
| `BLE:PAIR` | — | Initiate pairing/encryption on the connection |
| `BLE:PAIR:STATe?` | int | `0` idle, `1` in-progress, `2` passkey-needed, `3` numcmp-needed, `4` done, `5` failed, `6` display-key |
| `BLE:PAIR:PASSKey <n>` | — | Inject a 6-digit passkey (state `2`) |
| `BLE:PAIR:PASSKey?` | uint | Read the passkey we display (state `6`) |
| `BLE:PAIR:NUMCmp?` | uint | Read the number to compare (state `3`) |
| `BLE:PAIR:CONFirm <0\|1>` | — | Accept (`1`) / reject (`0`) numeric comparison |
| `BLE:SEC?` | CSV | `<mac>,<level>,<encrypted>,<authenticated>,<bonded>,<key_size>` |

`<level>` is the LE security level: `1` none, `2` encrypted/unauthenticated
(Just Works), `3` encrypted/authenticated, `4` plus a 128-bit LE SC key.

> A scan must stop before connecting (`BLE:CONNect` cancels any active
> discovery). Only one connection is tracked at a time.

> Pairing needs a bond store: `ble_conn_init()` calls `ble_store_config_init()`.
> Without it, `BLE:PAIR` fails with status `8` (`BLE_HS_ENOTSUP`) *before* any
> SMP packet is sent, because the store read callback is NULL.

#### Verified end-to-end (Raspberry Pi peer)

Pairing a Raspberry Pi (BlueZ) as the peripheral, over LE:

```bash
# On the Pi: advertise connectable over LE (the phone's BT settings page uses
# classic BR/EDR, which the ESP32-S3 cannot see — it is LE-only).
sudo btmgmt -i hci0 connectable on
sudo btmgmt -i hci0 bondable on
sudo btmgmt -i hci0 advertising on
# Register an agent so BlueZ can answer the pairing (e.g. KeyboardDisplay);
# without one BlueZ drops the link mid-pairing (HCI reason 0x13).
```

A successful numeric-comparison pairing yields, for example:

```
BLE:SEC? -> D8:3A:DD:E4:7A:98,4,1,1,1,16
            <mac>,level=4(LE SC auth),enc=1,auth=1,bonded=1,key=16B
```

### Hardware validation

With the bundled firmware flashed and a test AP running, the toolkit hardware
check uses the real Rust USB transport and a 1,000-entry list. Supply
`IOTSPLOIT_TEST_SERIAL`, `IOTSPLOIT_WIFI_SSID`, and `IOTSPLOIT_WIFI_PASSWORD`,
then run from `ui/`:

```bash
fvm flutter test --no-pub test/hardware/esp32_wifi_connection_hardware_test.dart --tags hardware
```

The check rejects the first credential, connects with the second, leaves the
remaining 998 untried, and verifies Stop and USB disconnect. It requires the
local debug `librust_lib_admin.so` bundle. Without the explicit environment
variables it is skipped.

Validated on ESP32-S3 `34851841C6AC` with firmware 1.2.0 using a temporary
WPA2 AP on the host PC:

- Wrong password returned `AUTH_FAILED`; correct password obtained DHCP and
  answered TCP SCPI on port 5025.
- Immediate Stop, established Stop, and connecting after cancellation passed.
- A missing AP returned `AP_NOT_FOUND`; an AP without DHCP returned
  `DHCP_TIMEOUT` after about 15 seconds. A subsequent connection passed.
- The real toolkit accepted 1,000 candidates, connected at candidate 2, skipped
  the remaining 998, and closed the USB session after Stop.
- AP restart ended the bounded reconnect without cycling candidates. A fresh
  connection passed; there is no indefinite background reconnect loop.

The temporary AP was deleted and the host PC's original hotspot restored.
