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
