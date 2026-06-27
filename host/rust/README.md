# iotsploit-host (Rust)

A command-line tool that lets you control any **`iotsploit-usb`** device from
your PC over a standard USB cable. The device speaks **SCPI over USBTMC**; this
host talks to it through the Linux kernel's `/dev/usbtmcN` driver, so you do
not need any board-specific code on the PC — one binary controls every board.

> This implements **Milestones 0–10** of the host plan
> ([`docs/iotsploit-usb-rust-host-plan-updated.md`](../../docs/iotsploit-usb-rust-host-plan-updated.md)):
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

- Discover and open any `iotsploit-usb` device automatically.
- Send SCPI commands and read text responses.
- Read **binary block** responses (e.g. `DATA:READ?`) without corrupting them
  as text.
- Parse `*IDN?`, `SYSTem:CAPabilities?`, and `SYSTem:HELP:HEADers?`.
- Query the on-device **line-record descriptor** (`SYSTem:HELP:DESCription?`)
  to discover commands, parameters, and workflows.
- Run **generic workflows** (`trigger → poll → count → fetch` or interactive
  polling) from command/workflow metadata read **live from the device** — there
  are no local profile files to keep in sync.
- Drain the SCPI error queue.
- Drive everything from a one-liner or an interactive prompt.

It is **device-independent**: it learns what commands exist from the device
itself, and profiles provide metadata for workflow automation. The same
binary works for the nRF52840, ESP32-S3, Pico2, and any future
`iotsploit-usb` board.

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

The binary is at `target/release/iotsploit-host`. The default (Linux kernel)
build has **zero external dependencies**. The `raw-usb` feature adds `nusb`
(pure Rust — no libusb C library to install) plus `futures-lite`/`async-io`;
it is needed on Windows/macOS and optional on Linux.

**Platform-specific build:**

```sh
# Linux (default: kernel backend, no extra deps)
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
sudo iotsploit-host idn
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

```sh
$ iotsploit-host list          # show USBTMC nodes without opening one
/dev/usbtmc0

$ iotsploit-host idn           # who are you?
IoTSploit,nRF52840,0001,0.1.0

$ iotsploit-host caps          # what can you do?
raw      : proto=1;mtu=256;maxblock=4096;feat=
proto    : Some(1)
mtu      : Some(256)
maxblock : Some(4096)
features : []

$ iotsploit-host headers       # list every SCPI command the device knows
*IDN?
*RST
*CLS
...
DATA:READ?
BLE:SCAN:START
BLE:SCAN:STOP
...
```

If `list` shows exactly one node, every other command auto-detects it — no
`--device` needed.

Profiles and workflows (metadata comes from the connected device):

```sh
$ iotsploit-host profile                # show the device's commands & workflows
idn      : IoTSploit,ESP32S3,0001,0.1.0
commands : 22
  GPIO:SET [command] Set GPIO output level
  ...
workflows: 3
  wifi-scan (TriggerPollFetch) Scan for Wi-Fi access points

$ iotsploit-host workflow wifi-scan     # run a workflow
trigger: WLAN:SCAN
done after 1.4s
results (2):
  [0] MyWiFi,-52,cc:cc:cc:cc:cc:cc
  [1] GuestNet,-78,aa:bb:cc:dd:ee:ff

$ iotsploit-host describe               # on-device descriptor (if supported)
DEV name=esp32s3 idn="IoTSploit,ESP32S3,0001,0.1.0" proto=1 mtu=256 max_block=4096
CMD GPIO:SET kind=command summary="Set GPIO output level" param=pin:u32:req param=value:bool:req returns=none
...
WF wifi-scan type=trigger_poll_fetch trigger=WLAN:SCAN done=WLAN:SCAN:DONE?:1 count=WLAN:SCAN:COUNt? fetch=WLAN:SCAN?#index
```

On Windows/macOS, use the raw USB backend with `--backend raw`:

```sh
iotsploit-host --backend raw --vid 1209 --pid 0001 idn
```

## 6. Command reference

General form:

```
iotsploit-host [--device <path>] <command> [args]
```

`--device /dev/usbtmc0` selects a specific node when several are present.

| Command | What it does |
|---|---|
| `list` | Print every `/dev/usbtmc*` node (Linux only, does not open the device). |
| `idn` | Query `*IDN?` and print the identity string. |
| `caps` | Query `SYSTem:CAPabilities?`, parse it, print structured fields. |
| `headers` | Fetch and list all command headers (`SYSTem:HELP:HEADers?`). |
| `describe` | Query `SYSTem:HELP:DESCription?` (line-record descriptor). |
| `query '<cmd>'` | Send any SCPI query, print its text response. |
| `write '<cmd>'` | Send a non-query SCPI command (no response printed). |
| `block-read '<cmd>' [--out <file>]` | Query a binary-block response and write the payload to a file (or stdout). |
| `errors` | Drain the `SYSTem:ERRor?` queue until "No error". |
| `workflow <name> [params]` | Run a device-described workflow (e.g. `wifi-scan`, `ble-scan`). |
| `profile` | Show the connected device's full command/workflow descriptor. |
| `repl` | Interactive SCPI prompt. |
| `-h, --help` | Show built-in help. |
| `-V, --version` | Show version. |

### Examples

```sh
# Text query
iotsploit-host query '*IDN?'
iotsploit-host query 'BLE:SCAN:STATe?'

# Non-query command (returns quickly; keeps the link in sync)
iotsploit-host write 'BLE:SCAN:CLEar'

# Binary block: payload goes to a file (raw bytes, never text-decoded)
iotsploit-host block-read 'DATA:READ? 64' --out adc.bin
ls -l adc.bin            # size == number of payload bytes

# Binary block to stdout (pipe into another tool)
iotsploit-host block-read 'DATA:READ? 64' | od -A x -t x1

# Error queue
iotsploit-host errors
```

> Quoting: SCPI commands usually contain `:` or `?`, so wrap them in single
> quotes (`'*IDN?'`) to protect them from the shell.

## 7. Interactive mode (REPL)

`repl` opens a prompt where each line is sent as one SCPI message:

```
$ iotsploit-host repl
iotsploit-host repl - type SCPI commands, Ctrl-D to exit
> *IDN?
IoTSploit,nRF52840,0001,0.1.0
> SYST:CAP?
proto=1;mtu=256;maxblock=4096;feat=
> BLE:SCAN:CLEar
ok
> BLE:SCAN:STATe?
0
> quit
```

Rules inside the REPL:
- A line ending in `?` is sent as a **query** and the text response is printed.
- Any other line is sent as a **command** and `ok` is printed.
- `exit` / `quit` / Ctrl-D leaves the REPL.
- Errors are printed but do not end the session.

The REPL is great for poking at a device, but for automation prefer the
one-liner commands above (they are easy to script).

## 8. Worked example: a BLE scan on the nRF52840

The nRF52840 example firmware exposes a small BLE-scan command set. The
built-in `nrf52840` profile knows how to drive it:

```text
> BLE:SCAN:CLEar        # forget previous results
ok
> BLE:SCAN:START        # begin scanning
ok
> BLE:SCAN:STATe?       # 1 = still scanning, 0 = idle/finished
1
> BLE:SCAN:STATe?
0
> BLE:SCAN:COUNt?       # how many devices were seen
3
> BLE:SCAN:RESult? 0    # addr,rssi,name
AA:BB:CC:DD:EE:FF,-67,MySensor
> BLE:SCAN:RESult? 1
11:22:33:44:55:66,-81,(unknown)
> BLE:SCAN:RESult? 2
DE:AD:BE:EF:00:01,-55,Headphones
> BLE:SCAN:STOP         # optional: stop early
ok
> errors
(no errors)
```

Or with the descriptor-driven workflow engine (does all the polling for you):

```sh
# Fetch the device descriptor, trigger scan, poll until done, fetch all results
iotsploit-host workflow ble-scan
# output:
# trigger: BLE:SCAN:START
# done after 3.2s
# results (3):
#   [0] AA:BB:CC:DD:EE:FF,-67,MySensor
#   [1] 11:22:33:44:55:66,-81,(unknown)
#   [2] DE:AD:BE:EF:00:01,-55,Headphones
```

You can inspect the connected device's full descriptor:

```sh
iotsploit-host profile                # commands + workflows from the device
```

## 9. How it works

```
your shell ──► iotsploit-host (Rust) ──► Transport trait
                                           │
                          ┌────────────────┼────────────────┐
                          │                │                │
                  /dev/usbtmcN      nusb (raw USB)    (future TCP)
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
- **Session** (`session.rs`): appends the SCPI `\n` terminator, trims trailing
  CR/LF from text, and never decodes binary blocks as text.
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

**`permission denied opening /dev/usbtmc0`** — the node is root-only. Use
`sudo`, or set up the udev rule in [§4](#4-granting-usb-access-one-time).

**`no /dev/usbtmc* device found`** — the board is not enumerated. Check
`lsusb` for `1209:0001`, re-plug the cable, and confirm the firmware is
flashed and running.

**`found N usbtmc devices`** — more than one USBTMC device is attached. Pick
one explicitly: `iotsploit-host --device /dev/usbtmc1 idn`.

**`caps` shows `mtu = None` and a `warning: unparseable numeric fields: mtu="zu"`**
— expected on some boards (e.g. the nRF52840) whose C library prints the `%zu`
format specifier literally as `zu`. The parser reports it as a warning and
falls back to safe defaults instead of failing.

**`block-read` writes a 0-byte file** — the command succeeded, but the device
returned an empty block. On the nRF52840, `DATA:READ?` is empty because no
data source is wired up in that example firmware; the path itself is correct.

**A `query` prints an empty line** — the command likely errored on the device
(unknown header, bad parameter, …). Run `iotsploit-host errors` to see the
SCPI error queue.

## 11. Testing

Unit + fake-transport integration tests (no hardware needed):

```sh
cargo test          # 61 tests, zero external deps
```

Hardware smoke test (nRF52840, after granting access):

```sh
iotsploit-host idn
iotsploit-host caps
iotsploit-host headers
iotsploit-host describe
iotsploit-host workflow ble-scan
iotsploit-host query 'BLE:SCAN:STATe?'
iotsploit-host block-read 'DATA:READ? 64' --out /tmp/adc.bin
iotsploit-host errors
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
sudo iotsploit-host --backend raw --vid 1209 --pid 0001 idn
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
   iotsploit-host.exe --backend raw --vid 1209 --pid 0001 idn
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
   iotsploit-host --backend raw --vid 1209 --pid 0001 idn
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
| `--backend ... idn` works | ✓ | ✓ | ▢ | ▢ |
| `headers` / `describe` | ✓ | ✓ | ▢ | ▢ |
| `query '*IDN?'` matches expected IDN | ✓ | ✓ | ▢ | ▢ |
| `block-read 'DATA:READ? 64' --out f` | ✓ | ✓ | ▢ | ▢ |
| `workflow wifi-scan` (esp32s3) | ✓ | ✓ | ▢ | ▢ |
| `workflow ble-scan` (nrf52840) | ✓ | ▢ | ▢ | ▢ |
