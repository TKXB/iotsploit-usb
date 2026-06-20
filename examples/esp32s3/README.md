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
