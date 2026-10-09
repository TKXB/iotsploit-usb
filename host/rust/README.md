# iotsploit-host (Rust)

A command-line tool that lets you control any **`iotsploit-usb`** device from
your PC, over USB or the network. The device speaks **SCPI** (over USBTMC or raw
TCP on port 5025) and describes its own commands and workflows, so you do not
need any board-specific code on the PC: one binary controls every board.

```sh
iotsploit-host devices            # what's connected
iotsploit-host info               # what this board is
iotsploit-host help               # what it can do
iotsploit-host send GPIO:SET 2,1  # do one thing
iotsploit-host workflow wifi-scan # run a multi-step job
```

> This implements **Milestones 0–10** of the host plan
> ([`docs/iotsploit-usb-rust-host-plan.md`](../../docs/iotsploit-usb-rust-host-plan.md)):
> a dependency-free core with text query/write, IEEE 488.2 binary-block
> support, capabilities parsing, command-header discovery, line-record
> profiles, workflow engine, device descriptor, and cross-platform raw
> USB backend.

---

## Table of contents

1. [What you can do with it](#1-what-you-can-do-with-it)
2. [Prerequisites](#2-prerequisites)
3. [Build](#3-build)
4. [Granting USB access (one-time)](#4-granting-usb-access-one-time)
5. [Quick start](#5-quick-start)
6. [Command reference](#6-command-reference)
7. [Interactive mode (REPL)](#7-interactive-mode-repl)
8. [Worked example: a BLE scan on the nRF52840](#8-worked-example-a-ble-scan-on-the-nrf52840)
9. [How it works](#9-how-it-works)
10. [Troubleshooting](#10-troubleshooting)
11. [Testing](#11-testing)
12. [Platform setup](#12-platform-setup)

---

## 1. What you can do with it

- List every connected board with its identity, and pick one by number.
- See what a board is (`info`) and what it can do (`help`), straight from the
  board's own description of its commands and workflows.
- Send any SCPI command (`send`). Text, binary blocks and errors are handled
  for you: if the device rejects a command you see why, with a suggestion for
  a mistyped name, and the exit status says it failed.
- Run multi-step **workflows** (`trigger → poll → fetch`, or interactive ones
  such as BLE pairing) that the device describes.
- Read a device's continuous data stream (`stream`): over TCP, or over the USB vendor pipe on USB-only boards.
- Drive everything from one-liners or an interactive prompt.

It is **device-independent**: the same binary works for the nRF52840,
ESP32-S3, Pico2, STM32F4 and any future `iotsploit-usb` board.

## 2. Prerequisites

- **Linux**, **Windows**, or **macOS** PC.
  - Linux: uses the kernel `/dev/usbtmcN` driver by default (no extra deps).
  - Windows/macOS: uses the raw USB backend (`nusb`, pure Rust — no libusb to
    install). See [Platform setup](#platform-setup) below.
- The **Rust toolchain** (`rustc` + `cargo`). Any recent stable version works.
  ```sh
  rustc --version   # e.g. 1.93.1
  ```
- An `iotsploit-usb` device flashed and plugged in over USB. On Linux it
  appears as `/dev/usbtmc0` (or `usbtmc1`, …). On Windows/macOS the raw
  backend auto-detects it by VID/PID.

Check that the OS sees it:

```sh
# Linux
ls -l /dev/usbtmc*
lsusb | grep 1209:0001

# macOS
system_profiler SPUSBDataType | grep -A5 1209

# Windows (PowerShell)
# Use Zadig to bind WinUSB first — see Platform setup below
```

## 3. Build

```sh
cd host/rust
cargo build --release
```

The binary is at `target/release/iotsploit-host`. The default build (Linux
kernel USBTMC plus TCP) has **zero external dependencies**. The `raw-usb` feature adds `nusb`
(pure Rust — no libusb C library to install) plus `futures-lite`/`async-io`;
it is needed on Windows/macOS and optional on Linux.

**Platform-specific build:**

```sh
# Linux (default: kernel USBTMC + TCP, no extra deps)
cargo build --release

# Windows / macOS / Linux (raw USB backend via nusb)
cargo build --release --features raw-usb

# Linux with both backends (kernel + raw)
cargo build --release --all-features
```

For convenience you can copy it onto your `PATH`:

```sh
cp target/release/iotsploit-host ~/.local/bin/
```

(The rest of this guide uses `iotsploit-host` to mean that binary.)

## 4. Granting USB access (one-time)

The USBTMC device node is root-only by default. You have two options.

**Option A — run with `sudo` (quickest):**

```sh
sudo iotsploit-host info
```

**Option B — a udev rule (recommended, so you never need `sudo`):**

Create `/etc/udev/rules.d/49-iotsploit-usbtmc.rules`:

```
# iotsploit-usb devices (VID 1209, PID 0001) -> accessible to plugdev users
SUBSYSTEM=="usb", ATTR{idVendor}=="1209", ATTR{idProduct}=="0001", MODE="0660", GROUP="plugdev"
KERNEL=="usbtmc*", MODE="0660", GROUP="plugdev"
```

Then reload and re-plug the board:

```sh
sudo udevadm control --reload-rules
sudo udevadm trigger
# unplug/replug the USB cable, or: sudo udevadm trigger --action=add
```

Make sure your user is in the `plugdev` group (`groups` should list it; if not,
`sudo usermod -aG plugdev $USER` and log out/in). After that `iotsploit-host`
runs as your normal user.

## 5. Quick start

```console
$ iotsploit-host devices
#  ADDRESS   IDENTITY
1  usbtmc:0  IoTSploit,nRF52840,6F1A2B3C4D5E6F70,0.1.0
2  usbtmc:1  IoTSploit,ESP32S3,34851841C6AC,0.1.0

$ iotsploit-host -d 2 info
device     IoTSploit,ESP32S3,34851841C6AC,0.1.0
address    usbtmc:1
protocol   1   mtu 256   max block 4096
commands   34   workflows 5   stream yes

$ iotsploit-host -d 2 help
COMMANDS (send with: iotsploit-host send <command>)
  GPIO:SET <pin>,<value>   Set GPIO output level
  ...
WORKFLOWS (run with: iotsploit-host workflow <name>)
  wifi-scan  Scan for Wi-Fi access points
  ...

$ iotsploit-host -d 2 send GPIO:SET 2,1
ok

$ iotsploit-host -d 2 workflow wifi-scan
found 2 result(s):
  [ 0] MyWiFi,-52,6,WPA2_PSK,cc:cc:cc:cc:cc:cc
  [ 1] GuestNet,-78,11,OPEN,aa:bb:cc:dd:ee:ff
```

With exactly one board connected, `-d` is not needed. To stop typing it, set
`IOTSPLOIT_DEVICE` (for example `export IOTSPLOIT_DEVICE=tcp://192.168.4.1`).

## 6. Command reference

```
iotsploit-host [-d <device>] <command> [args]
```

| Command | What it does |
|---|---|
| `devices` | List connected USB devices, numbered, with their `*IDN?`. |
| `info` | Identity, address, capabilities, and how many commands and workflows the device describes. |
| `help [name]` | Every command and workflow the device describes; with a name, its parameters, types and summary. Names may be long or short form, in any case (`help gpio:set`). |
| `send <command>` | Send one SCPI command. Prints a text reply, writes a binary block to stdout (or `-o <file>`), prints `ok` on stderr for a command with no reply, or prints the device's error. |
| `workflow <name> [params]` | Run a workflow the device describes (`help <name>` shows its parameters). |
| `stream [count]` | Start the capture and print records from the data plane (TCP socket or USB vendor pipe) until `count` or Ctrl-C. |
| `repl` | Interactive prompt; each line is sent like `send`. |
| `errors` | Read and clear the device's error queue. |

| Option | Meaning |
|---|---|
| `-d, --device <dev>` | A number from `devices`, `usbtmc:N` (or `/dev/usbtmcN`), `usb:<vid>:<pid>[/<serial>]` (hex), or `tcp://<host>[:<port>]`. Default: `$IOTSPLOIT_DEVICE`, else the only connected USB device. |
| `-o, --out <file>` | Save a binary reply to a file. |
| `--timeout <ms>` | Read timeout for TCP and raw USB (default 5000). |

Exit status: `0` ok, `1` the device reported an error, `2` usage error,
`3` could not connect.

### Examples

```sh
iotsploit-host send '*IDN?'
iotsploit-host send GPIO:SET 2,1            # unquoted arguments are joined
iotsploit-host send 'DATA:READ? 64' -o adc.bin
iotsploit-host send 'DATA:READ? 64' | od -A x -t x1
iotsploit-host help ble-scan
iotsploit-host -d tcp://10.42.0.57 workflow wifi-scan
```

> Quoting: a command ending in `?` can match files in some shells, so quote
> queries (`'*IDN?'`).

When the device rejects a command, `send` says why and exits with status 1:

```console
$ iotsploit-host send GPIO:GTE? 2
error -113 Undefined header: GPIO:GTE?
  did you mean `GPIO:GET?`
```

`send` asks for the error queue in the same message as the command
(`*CLS;<command>;:SYSTem:ERRor?`), so checking costs no extra round trip and a
rejected query fails at once instead of waiting for a reply that never comes.

### Older names

These still work, print the name that replaces them, and will be removed in a
later release:

| Old | New |
|---|---|
| `list` | `devices` |
| `idn`, `caps` | `info` |
| `headers`, `describe`, `profile` | `help` |
| `query`, `write`, `block-read` | `send` |
| `--backend`, `--vid`, `--pid`, `--serial` | `-d` |

## 7. Interactive mode (REPL)

`repl` opens a prompt where each line is sent like `send`:

```
$ iotsploit-host repl
connected to usbtmc:0; type SCPI commands, `quit` or Ctrl-D to exit
> *IDN?
IoTSploit,nRF52840,6F1A2B3C4D5E6F70,0.1.0
> BLE:SCAN:CLEar
ok
> BLE:SCAN:STAT?
0
> BLE:SCAN:STRT
error -113 Undefined header: BLE:SCAN:STRT
  did you mean `BLE:SCAN:START`
> quit
```

Errors are printed but do not end the session. For automation prefer the
one-line commands, which are easy to script.

## 8. Worked example: a BLE scan on the nRF52840

```console
$ iotsploit-host help ble-scan
ble-scan
  Scan for BLE devices

  usage: iotsploit-host workflow ble-scan [duration]
    duration  u32  optional

  results: addr, rssi (dbm), name, conn
  starts with BLE:SCAN; gives up after 30 s

$ iotsploit-host workflow ble-scan 8
found 3 result(s):
  [ 0] AA:BB:CC:DD:EE:FF,-67,MySensor,C
  [ 1] 11:22:33:44:55:66,-81,(unknown),N
  [ 2] DE:AD:BE:EF:00:01,-55,Headphones,C
```

The same scan by hand, one command at a time:

```text
> BLE:SCAN:CLEar        # forget previous results
ok
> BLE:SCAN:START        # begin scanning
ok
> BLE:SCAN:STATe?       # 1 = still scanning, 0 = idle/finished
0
> BLE:SCAN:COUNt?       # how many devices were seen
3
> BLE:SCAN:RESult? 0    # addr,rssi,name,conn
AA:BB:CC:DD:EE:FF,-67,MySensor,C
```

## 9. How it works

```
your shell ──► iotsploit-host (Rust) ──► Transport trait
                                           │
                          ┌────────────────┼────────────────┐
                          │                │                │
                  /dev/usbtmcN      nusb (raw USB)    TCP :5025
                  Linux kernel       Win / mac / Linux
                                           │
                                  USB cable (USBTMC)
                                           │
                                  iotsploit-usb firmware
                                  (libscpi + TinyUSB)
```

- **Transport** (`transport.rs`): an abstract `write_msg` / `read_msg` trait.
  - `usbtmc_kernel.rs`: Linux `/dev/usbtmcN` backend (default, zero-dep).
  - `usbtmc_raw.rs`: raw USBTMC bulk transfers via `nusb` (pure Rust,
    `--features raw-usb`). Used on Windows/macOS and optionally on Linux.
  - `tcp.rs`: raw SCPI over TCP, restoring message boundaries from the stream.
- **Discover** (`discover.rs`): the `-d` address syntax and the `devices` list.
- **Data plane** (`dataplane.rs`, `framing.rs`): finds the record stream
  (`SYSTem:STReam:FORMat?`, port 0 = USB only), reads it from a TCP socket, or
  decodes REC frames from the USB vendor pipe (`UsbRecordStream` in
  `usbtmc_raw.rs`), and tracks the in-band `dropped` counter.
- **Session** (`session.rs`): appends the SCPI `\n` terminator, trims trailing
  CR/LF from text, never decodes binary blocks as text, and runs a command
  together with its error check (`send_checked`).
- **Block** (`block.rs`): parses/encodes IEEE 488.2 definite-length arbitrary
  blocks (`#<digits><len><payload>`), preserving arbitrary bytes.
- **Caps** (`caps.rs`): tolerantly parses `SYSTem:CAPabilities?`.
- **Headers** (`headers.rs`): lists commands; falls back to paging for large
  command sets.
- **Descriptor** (`descriptor.rs`): line-record parser for the on-device
  `SYSTem:HELP:DESCription?` response (the same parser also reads a file via
  `load_file`, used in tests). Zero-dependency — plain `std`, no
  `serde`/`toml`/`serde_json`.
- **Workflow** (`workflow.rs`): generic `trigger → poll → count → fetch`
  engine driven by profile/descriptor metadata.

The default (kernel) build is dependency-free on purpose. The `raw-usb`
feature adds `nusb` (pure Rust, no libusb) plus `futures-lite`/`async-io`.

## 10. Troubleshooting

**`devices` shows `(cannot open: … permission denied …)`** — the node is
root-only. Use `sudo`, or set up the udev rule in
[§4](#4-granting-usb-access-one-time).

**`no USB device found`** — the board is not enumerated. Check `lsusb` for
`1209:0001`, re-plug the cable, and confirm the firmware is flashed and
running. Network devices are never discovered; name them with
`-d tcp://<host>`.

**`2 devices connected; choose one with -d <number>`** — pick one from
`iotsploit-host devices`, for example `iotsploit-host -d 2 info`.

**`error -113 Undefined header`** — the device does not know that command.
`help` lists the ones it does; the error line suggests the closest match.

**`warning: unparseable capability fields: mtu="zu"`** — expected on some
boards (e.g. the nRF52840) whose C library prints the `%zu` format specifier
literally as `zu`. The parser falls back to safe defaults instead of failing.

**A binary reply is 0 bytes** — the command succeeded, but the device returned
an empty block. On the nRF52840, `DATA:READ?` is empty because no data source
is wired up in that example firmware; the path itself is correct.

**`help` lists names only** — the firmware does not serve
`SYSTem:HELP:DESCription?`. Update it to get parameter details and workflows.

## 11. Testing

Unit + fake-transport integration tests (no hardware needed):

```sh
cargo test          # 106 tests, zero external deps
```

`tests/scpi_tcp_smoke.py` and the daemon cover the TCP path without hardware:

```sh
./build/examples/daemon/usbscpi_daemon 127.0.0.1 5025 &
iotsploit-host -d tcp://127.0.0.1 info
```

Hardware smoke test (after granting access):

```sh
iotsploit-host devices
iotsploit-host info
iotsploit-host help
iotsploit-host send '*IDN?'
iotsploit-host send 'DATA:READ? 64' -o /tmp/adc.bin
iotsploit-host workflow ble-scan      # or wifi-scan on the ESP32-S3
```

---

## 12. Platform setup

### Linux

The kernel `/dev/usbtmcN` backend works out of the box. For non-root access,
install the udev rule from [§4](#4-granting-usb-access-one-time).

For the raw USB backend (optional, useful when the kernel driver is
unavailable). `nusb` talks to usbfs directly, so no libusb package is needed —
just permission to access the device (the same udev rule, or run with `sudo`):

```sh
cargo build --release --features raw-usb
sudo iotsploit-host -d usb:1209:0001 info
```

`nusb` detaches the kernel `usbtmc` driver automatically when it claims the
interface.

### Windows

Windows has no `/dev/usbtmcN` equivalent. Use the raw USB backend:

1. Install the Rust toolchain (`rustup`).
2. Build with the raw-usb feature:
   ```sh
   cargo build --release --features raw-usb --target x86_64-pc-windows-msvc
   ```
3. **Driver binding** (one-time per device): use
   [Zadig](https://zadig.akeo.ie/) to bind the USBTMC interface to
   **WinUSB** (recommended) or **libusbK**.
   - In Zadig, select `Options → List All Devices`.
   - Find the `iotsploit-usb` device (VID 1209, PID 0001).
   - Set the driver to `WinUSB` and click `Replace Driver`.
4. Run:
   ```sh
   iotsploit-host.exe devices
   iotsploit-host.exe info
   ```

No NI-VISA dependency is required.

### macOS

macOS also needs the raw USB backend (`nusb` uses IOKit directly — no
Homebrew libusb required):

1. Build with the raw-usb feature:
   ```sh
   cargo build --release --features raw-usb
   # Apple Silicon:  aarch64-apple-darwin (default on M-series Macs)
   # Intel:          x86_64-apple-darwin
   ```
2. Run (the first time macOS will prompt for USB permission):
   ```sh
   iotsploit-host devices
   iotsploit-host info
   ```

For distribution, the binary should be code-signed and notarized. This is a
future packaging task; for development, running locally is fine.

### Release artifact naming

```text
iotsploit-host-linux-x86_64
iotsploit-host-windows-x86_64.exe
iotsploit-host-macos-x86_64
iotsploit-host-macos-aarch64
```

### Smoke-test checklist (per OS)

Legend: ✓ = verified on hardware; ▢ = expected to work but not yet verified on
that OS (the raw `nusb` path is identical across OSes apart from the platform
USB shim, so the Linux ✓ exercises the same code).

| Check | Linux kernel | Linux raw (nusb) | Windows | macOS |
|---|---|---|---|---|
| `info` works | ✓ | ✓ | ▢ | ▢ |
| `help` | ✓ | ✓ | ▢ | ▢ |
| `send '*IDN?'` matches expected IDN | ✓ | ✓ | ▢ | ▢ |
| `send 'DATA:READ? 64' -o f` | ✓ | ✓ | ▢ | ▢ |
| `workflow wifi-scan` (esp32s3) | ✓ | ✓ | ▢ | ▢ |
| `workflow ble-scan` (nrf52840) | ✓ | ▢ | ▢ | ▢ |
