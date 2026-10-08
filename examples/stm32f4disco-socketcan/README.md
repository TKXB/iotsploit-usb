# STM32F4-Discovery SocketCAN Example

[`examples/stm32f4disco`](../stm32f4disco/README.md) plus a gs_usb
(candleLight) vendor interface, so the Linux `gs_usb` driver turns CAN1 and
CAN2 into native SocketCAN interfaces (`can0`, `can1`). The USBTMC/SCPI
instrument, its LED/GPIO/CAN commands, the hardware, wiring and toolchain are
those of the plain example; read its README first. The CMSIS shim, linker
script and OpenOCD config are used from that directory, not copied.

## USB Layout

| Interface | Class | Endpoints | Host driver |
|---|---|---|---|
| 0 | vendor (0xFF), gs_usb | bulk IN 0x81, OUT 0x02 | `gs_usb` |
| 1 | USBTMC / USB488 | bulk OUT 0x03, IN 0x83 | `usbtmc` |

gs_usb must be interface 0 with those endpoints: Linux sends its control
requests with wIndex 0, and kernels without endpoint discovery hard-code 0x81
and 0x02. VID:PID is 1209:0001, as in the plain example.

## Build and Flash

```bash
cd examples/stm32f4disco-socketcan
make -j$(nproc)    # TINYUSB_ROOT, LIBOPENCM3_DIR as in ../stm32f4disco
make flash
```

```text
   text    data     bss     dec     hex  filename
  48020     204   12884   61108    eeb4  stm32f4disco_socketcan.elf
```

## SocketCAN

The VID:PID is not in the driver's table, so bind it once per boot (`ff`
restricts the match to the vendor interface; USBTMC stays on `usbtmc`):

```bash
sudo modprobe gs_usb
echo 1209 0001 ff | sudo tee /sys/bus/usb/drivers/gs_usb/new_id
ip -br link | grep can            # can0 = CAN1, can1 = CAN2
sudo ip link set can0 up type can bitrate 500000
sudo ip link set can1 up type can bitrate 500000
candump can1 &
cansend can0 123#DEADBEEF         # can1  123   [4]  DE AD BE EF
```

To bind automatically, add a udev rule:

```text
# /etc/udev/rules.d/60-iotsploit-gs_usb.rules
ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="0001", RUN+="/bin/sh -c 'modprobe gs_usb; echo 1209 0001 ff > /sys/bus/usb/drivers/gs_usb/new_id'"
```

Bit timing comes from the kernel (42 MHz clock, tseg1 1-16, tseg2 1-8, sjw up
to 4, brp 1-1024), so any bitrate `ip link` can reach works, not only the four
`CAN:OPEN` rates. `listen-only on` and `loopback on` map to the bxCAN silent
and loopback modes. Frames go out in the order the host sent them, and each one
is echoed to the host once it has left the controller.

A gs_usb receive queue of 102 frames is separate from the SCPI one; frames lost
to it are reported to SocketCAN as RX overflow (`ip -s link`).

## Sharing a Bus with SCPI

A controller has one owner. `ip link set canN up` takes it, even from SCPI;
`CAN:OPEN` and `CAN:SEND` on that bus then fail with an execution error until
`ip link set canN down`. `CAN:STATe? <bus>` returns
`owner,tec,rec,busoff,rx_dropped` with owner 0 closed, 1 SCPI, 2 SocketCAN.

```bash
sudo iotsploit-host send "*IDN?"          # IoTSploit,STM32F4-Disco-SocketCAN,<chip serial>,0.1.0
sudo iotsploit-host send "CAN:STATe? 1"   # 2,0,0,0,0 while can0 is up
```

## Source Files

| File | Purpose |
|---|---|
| `main.c` | Board init, SCPI command callbacks, gs_usb protocol, main loop |
| `usb_descriptors.c` | Composite descriptors: gs_usb (interface 0) + USBTMC (interface 1) |
| `tusb_config.h` | TinyUSB configuration: USBTMC + vendor class |
| `Makefile` | Build system; board files come from `../stm32f4disco` |
