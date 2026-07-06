# STM32F4-Discovery USBTMC/SCPI Example

Bare-metal USBTMC + SCPI demo running on the STM32F4-Discovery board. The device enumerates as a USBTMC instrument over USB, accepts SCPI commands, and controls on-board LEDs and the user button.

## Tech Stack

| Layer | Library | Role |
|---|---|---|
| SCPI parser | libscpi v2.3 (vendored) | SCPI-99 command parsing, error queue, parameter handling |
| USBTMC core | iotsploit-usb | Stack-neutral USBTMC receive/respond path |
| USB device stack | TinyUSB | USB device framework with USBTMC class and DWC2 (Synopsys) driver |
| Board support | libopencm3 | Clock tree, GPIO, NVIC, vector table, interrupt routing |
| CMSIS shim | `stm32f4xx.h` (local) | Minimal compatibility header so TinyUSB's DWC2 driver builds without STM32Cube HAL |
| SCPI-over-USB glue | `usbscpi_tinyusb.c` | Bridges TinyUSB USBTMC callbacks to the iotsploit-usb core |

### Architecture

```
main.c                    usbscpi_tinyusb.c           usbscpi.c
(board init,              (TinyUSB USBTMC glue:       (SCPI core:
 SCPI command callbacks)   bulk-IN/OUT routing)         parse, respond)
       |                         |                          |
       v                         v                          v
libopencm3                TinyUSB DWC2 driver          libscpi v2.3
(clocks, GPIO,            (dcd_dwc2.c:                 (SCPI parser
 NVIC, vector table)       OTG FS register access)      library)
                                  |
                            stm32f4xx.h
                            (CMSIS shim for DWC2)
```

## Hardware

| Feature | Detail |
|---|---|
| Board | STM32F4-Discovery |
| MCU | STM32F407VGT6 (Cortex-M4F, 168 MHz) |
| USB | OTG FS full-speed, PA11 (DM) / PA12 (DP), AF10 |
| Clock | HSE 8 MHz -> PLL -> 168 MHz SYSCLK, 48 MHz PLLQ (USB) |
| LEDs | PD12 (green), PD13 (orange), PD14 (red), PD15 (blue) |
| Button | PA0 (user, active high) |
| Flash | 1 MB at 0x08000000 |
| SRAM | 128 KB at 0x20000000 (USB cannot access CCM at 0x10000000) |
| Debug | On-board ST-Link/V2 (SWD) |

## Toolchain

| Tool | Version | Purpose |
|---|---|---|
| arm-none-eabi-gcc | 14.2.1 | Cross-compiler for Cortex-M4 |
| libopencm3 | pre-built | ARM Cortex-M support library |
| TinyUSB | source | USB device stack (DWC2 driver) |
| OpenOCD | 0.12.0+ | Flashing via ST-Link/V2 SWD |
| GNU Make | any | Build system |

### Dependency Paths

The Makefile expects these paths (override with environment variables):

```bash
TINYUSB_ROOT  ?= ~/nordic/tinyusb
LIBOPENCM3_DIR ?= ~/Projects/libopencm3
```

## Build

```bash
cd examples/stm32f4disco
make -j$(nproc)
```

Output files in `_build/`:

| File | Description |
|---|---|
| `stm32f4disco_usbscpi.elf` | ELF with debug symbols (for flashing and GDB) |
| `stm32f4disco_usbscpi.bin` | Raw binary |
| `stm32f4disco_usbscpi.hex` | Intel HEX |

### Build Flags

```
CPU:     -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
Opt:     -O2 -g3
Defines: -DSTM32F4 -DSTM32F407VG -DCFG_TUSB_MCU=OPT_MCU_STM32F4 -DSCPI_USER_CONFIG=1
Std:     -std=c99
Link:    --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections
```

### Memory Usage

```text
   text    data     bss     dec     hex  filename
  40544     176    9364   50084    c3a4  stm32f4disco_usbscpi.elf

FLASH: 40,552 B / 1 MB  (3.87%)
RAM:    9,532 B / 128 KB (7.27%)
```

## Flash

Connect the mini-USB cable (ST-Link port) to the host, then:

```bash
make flash
```

This runs:

```bash
openocd -f openocd.cfg -c "program _build/stm32f4disco_usbscpi.elf verify reset exit"
```

The `openocd.cfg` configures ST-Link/V2 HLA mode over SWD at 2000 kHz.

### Manual Flash

```bash
openocd -f openocd.cfg -c "program _build/stm32f4disco_usbscpi.elf verify reset exit"
```

## Verify

After flashing, connect a micro-USB cable to the **USB OTG FS** port (not the ST-Link port). The device should enumerate:

```bash
lsusb | grep 1209:0001
# Bus 004 Device 003: ID 1209:0001 Generic pid.codes Test PID

ls /dev/usbtmc*
# /dev/usbtmc0
```

### Quick SCPI Test

```bash
sudo python3 -c "
import os
d = os.open('/dev/usbtmc0', os.O_RDWR)
os.write(d, b'*IDN?\n')
print(os.read(d, 4096).decode().strip())
"
# IoTSploit,STM32F4-Disco,0001,0.1.0
```

### Test with iotsploit-host

```bash
sudo iotsploit-host scpi "*IDN?"
# IoTSploit,STM32F4-Disco,0001,0.1.0

sudo iotsploit-host scpi "LED:GREen 1"
sudo iotsploit-host scpi "LED:ALL 1"
sudo iotsploit-host scpi "LED:TOGgle 2"
sudo iotsploit-host scpi "BTN?"
```

## SCPI Commands

| Command | Returns | Description |
|---|---|---|
| `*IDN?` | string | Device identity |
| `*RST` | — | Reset SCPI state |
| `*CLS` | — | Clear error queue |
| `LED:SET <n> <val>` | — | Set LED n (0-3) to val (0/1) |
| `LED:GET? <n>` | u32 | Read LED n state |
| `LED:GREen <val>` | — | Set green LED (PD12) |
| `LED:GREen?` | u32 | Read green LED state |
| `LED:ORAnge <val>` | — | Set orange LED (PD13) |
| `LED:ORAnge?` | u32 | Read orange LED state |
| `LED:RED <val>` | — | Set red LED (PD14) |
| `LED:RED?` | u32 | Read red LED state |
| `LED:BLUe <val>` | — | Set blue LED (PD15) |
| `LED:BLUe?` | u32 | Read blue LED state |
| `LED:ALL <val>` | — | Set all LEDs (0=off, 1=on) |
| `LED:ALL?` | u32 | Read all LEDs as bitmask (bit0=green..bit3=blue) |
| `LED:TOGgle <n>` | — | Toggle LED n (0-3) |
| `BTN?` | u32 | Read user button (PA0), 1=pressed |
| `GPIO:SET <pin> <val>` | — | Set GPIOA pin (0-15) |
| `GPIO:GET? <pin>` | u32 | Read GPIOA pin (0-15) |

## Source Files

| File | Purpose |
|---|---|
| `main.c` | Board init (clocks, GPIO, USB), SCPI command callbacks, main loop |
| `usb_descriptors.c` | USB device/config/string descriptors |
| `tusb_config.h` | TinyUSB compile-time configuration |
| `stm32f4xx.h` | CMSIS compatibility shim for TinyUSB's DWC2 driver |
| `linker.ld` | Linker script (1 MB flash, 128 KB SRAM) |
| `openocd.cfg` | OpenOCD config for ST-Link/V2 SWD |
| `Makefile` | Build system |

## Clean

```bash
make clean
```
