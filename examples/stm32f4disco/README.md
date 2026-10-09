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
| CAN1 | PD0 (RX) / PD1 (TX), AF9, external transceiver |
| CAN2 | PB12 (RX) / PB13 (TX), AF9, external transceiver |
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
# IoTSploit,STM32F4-Disco,<chip serial>,0.1.0
```

### Test with iotsploit-host

```bash
sudo iotsploit-host send "*IDN?"
# IoTSploit,STM32F4-Disco,<chip serial>,0.1.0

sudo iotsploit-host send "LED 0,1"        # green on
sudo iotsploit-host send "LED? 0"
sudo iotsploit-host send "LED:TOGgle 2"
sudo iotsploit-host send "BUTTon?"
```

## SCPI Commands

| Command | Returns | Description |
|---|---|---|
| `*IDN?` | string | Device identity |
| `*RST` | — | Reset SCPI state |
| `*CLS` | — | Clear error queue |
| `LED <n>,<val>` | — | Set LED n (0 green, 1 orange, 2 red, 3 blue) to val (0/1) |
| `LED? <n>` | u32 | Read LED n |
| `LED:TOGgle <n>` | — | Toggle LED n |
| `BUTTon?` | u32 | Read the user button (PA0), 1=pressed |
| `GPIO <pin>,<val>` | — | Set GPIOA pin (0-15) |
| `GPIO? <pin>` | u32 | Read GPIOA pin (0-15) |
| `CAN:OPEN <bus>,<bitrate>` | — | Start bus 1 or 2 at 125000/250000/500000/1000000 bit/s, accept all IDs |
| `CAN:SEND <bus>,<id>,"<hex>"` | — | Send one frame (0-8 data bytes); id > 0x7FF is sent as 29-bit extended |
| `CAN:STATe? <bus>` | string | `open,tec,rec,busoff,rx_dropped` |
| `SYSTem:STReam:STARt` | — | Stream received frames until STOP; refused unless a bus is open |
| `SYSTem:STReam:STOP` | — | Stop; frames already queued are still sent |
| `SYSTem:STReam:STATe?` | string | `running,attached` |
| `SYSTem:STReam:COUNt?` | u64 | Frames captured since STARt |
| `SYSTem:STReam:DROPped?` | u64 | Frames lost to a full buffer since STARt |
| `SYSTem:STReam:FORMat?` | string | `ver=1,stride=32,fields=...` |
| `SYSTem:STReam:FRAMing <0/1>` | — | Frame the vendor pipe; the host sends this |

`LED:SET`, `LED:GET?`, `BTN?`, `CAN:RECV?` and `CAN:COUNt?` still work as
undescribed aliases for one release.

## USB Layout

| Interface | Class | Endpoints | Host driver |
|---|---|---|---|
| 0 | USBTMC / USB488 | bulk OUT 0x01, IN 0x81 | `usbtmc` or raw |
| 1 | vendor (0xFF), subclass 0x49, protocol 0x53: CAN stream | bulk IN 0x82 (OUT 0x02 unused) | none (claimed by the host) |

## CAN

Wire each controller to its own 3.3 V CAN transceiver (SN65HVD230, TJA1051T/3,
MCP2562 with VIO = 3.3 V): MCU TX -> transceiver TXD, MCU RX -> transceiver RXD.
Tie the transceiver's standby/silent pin low and terminate each bus with 120 Ω
at both ends. CAN2 shares filter banks with CAN1, so the CAN1 clock is always
enabled; bus 1 uses filter bank 0 and bus 2 uses bank 14.

Bit timing is fixed at 14 time quanta (1 + 11 + 2, sample point 85.7 %) from
the 42 MHz APB1 clock. `CAN:OPEN` fails with an execution error when the
controller cannot leave init mode, which usually means no transceiver is
connected.

### Capturing

SCPI sets the bus up and starts and stops the capture; the frames themselves
stream over the CAN stream interface, one 32-byte record each
(`can_stream.h`), until you stop. There is no frame limit and nothing to poll.

```bash
sudo iotsploit-host send "CAN:OPEN 1,500000"
sudo iotsploit-host stream          # FRAMing 1 + STARt; Ctrl-C or a count ends it
sudo iotsploit-host stream 100      # stop after 100 frames
sudo iotsploit-host send 'CAN:SEND 1,#H123,"DEADBEEF"'
```

`stream` prints one hex record per line and reports gaps from the in-band
`dropped` counter. Ctrl-C leaves the capture running; send
`SYSTem:STReam:STOP` (or reconnect: an unplug resets it). In the GUI, open the
Data Plane tab: Start / Stop drive the capture and Record writes the decoded
frames to a CSV file.

Record fields: `ts_us` (µs since boot, taken in the RX interrupt), `dropped`
(frames lost to a full 256-record buffer since STARt, at capture), `can_id`,
`len`, `flags` (bit 0 extended, bit 1 RTR), `bus` (1 or 2), `data` (8 bytes,
`len` valid). Without the stream running, received frames go to a 64-frame
queue that the `CAN:RECV?` alias pops.

Quote the data: unquoted hex that starts with a digit parses as a number.

### Hardware Checklist

Not yet run on hardware. Two boards (or CAN1 wired to CAN2 through two
transceivers) and a second sender such as `cansend` on a USB-CAN adapter:

1. **Contents.** `CAN:OPEN 1,500000`, start in the GUI, send standard
   (`#H123`), extended (`#H1ABCDEF0`), zero-length and 8-byte frames. Each row
   shows the same `can_id`, `len`, `flags` and `data`.
2. **Ordering.** Send 1000 frames with an incrementing payload
   (`cangen -I 100 -D i -n 1000`); the recorded CSV has them in order, `ts_us`
   rising.
3. **Drops.** Flood at 1 Mbit/s (`cangen -g 0`) for 10 s. Either the lost count
   stays 0, or `SYSTem:STReam:DROPped?` equals the GUI's lost count and the CSV
   shows the jump in `dropped` where it happened.
4. **Quiet bus.** Start with nothing on the bus for a minute: no timeout, no
   error; frames sent afterwards appear.
5. **Stop / restart.** Stop, send frames (they must not appear), Start again:
   counts restart from 0 and new frames appear. Repeat ten times.
6. **USB recovery.** Unplug during a capture: the GUI reports the stream
   closed. Replug and reconnect: `SYSTem:STReam:STATe?` is `0,0`, and Start
   works without resetting the board.
7. **SocketCAN board.** With `can0` up through gs_usb, streaming bus 2 must
   not take `can0` down, and `ip -details link show can0` stays `ERROR-ACTIVE`.

## Source Files

| File | Purpose |
|---|---|
| `main.c` | Board init (clocks, GPIO, USB), SCPI command callbacks, main loop |
| `usb_descriptors.c` | USB device/config/string descriptors |
| `can_stream.h` | CAN stream: record, ring, SysTick timestamps, `SYSTem:STReam:*` (shared with the SocketCAN board) |
| `can_stream_itf.h` | The stream interface's USB descriptor |
| `tusb_config.h` | TinyUSB compile-time configuration |
| `stm32f4xx.h` | CMSIS compatibility shim for TinyUSB's DWC2 driver |
| `linker.ld` | Linker script (1 MB flash, 128 KB SRAM) |
| `openocd.cfg` | OpenOCD config for ST-Link/V2 SWD |
| `Makefile` | Build system |

## Clean

```bash
make clean
```
