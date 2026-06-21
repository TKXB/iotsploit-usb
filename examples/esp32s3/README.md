# ESP32-S3 USBTMC Demo

ESP32-S3 USBTMC (Test and Measurement Class) demo using TinyUSB.

## Build

```bash
. /home/tkxb/HDD/Projects/esp-idf/export.sh
cd /home/tkxb/Projects/iotsploit-usb/examples/esp32s3
idf.py set-target esp32s3
idf.py build
```

## Flash & Monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

## Host Test (pyvisa)

```python
import pyvisa
d = pyvisa.ResourceManager().open_resource('USB0::0x1209::0x0001::0001::INSTR')
print(d.query('*IDN?'))
print(d.query('SYST:CAP?'))
d.write('GPIO:SET 2,1')
print(d.query('GPIO:GET? 2'))
print(d.query('ADC:READ? 0'))
print(d.query_binary_values('DATA:READ? 64', datatype='B'))
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

The `host/scan_test.py` helper wraps this: `sudo python3 scan_test.py --connect 0`.

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
