# iotsploit-host (Rust)

A command-line tool that lets you control any **`iotsploit-usb`** device from
your PC over a standard USB cable. The device speaks **SCPI over USBTMC**; this
host talks to it through the Linux kernel's `/dev/usbtmcN` driver, so you do
not need any board-specific code on the PC — one binary controls every board.

> This implements **Milestones 0–3** of the host plan
> ([`docs/iotsploit-usb-rust-host-plan.md`](../../docs/iotsploit-usb-rust-host-plan.md)):
> a dependency-free core with text query/write, IEEE 488.2 binary-block
> support, capabilities parsing, and command-header discovery. Profiles,
> workflows, and the device descriptor arrive in later milestones.

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

---

## 1. What you can do with it

- Discover and open any `/dev/usbtmc*` device automatically.
- Send SCPI commands and read text responses.
- Read **binary block** responses (e.g. `DATA:READ?`) without corrupting them
  as text.
- Parse `*IDN?`, `SYSTem:CAPabilities?`, and `SYSTem:HELP:HEADers?`.
- Drain the SCPI error queue.
- Drive everything from a one-liner or an interactive prompt.

It is **device-independent**: it learns what commands exist from the device
itself (`SYSTem:HELP:HEADers?`), so the same binary works for the nRF52840,
ESP32-S3, Pico2, and any future `iotsploit-usb` board.

## 2. Prerequisites

- A **Linux** PC (the backend uses the kernel USBTMC character device).
- The **Rust toolchain** (`rustc` + `cargo`). Any recent stable version works.
  ```sh
  rustc --version   # e.g. 1.93.1
  ```
- An `iotsploit-usb` device flashed and plugged in over USB. After enumeration
  it appears as `/dev/usbtmc0` (or `usbtmc1`, …).

Check that the kernel sees it:

```sh
ls -l /dev/usbtmc*
# crw------- 1 root root 180, 176 ... /dev/usbtmc0

lsusb | grep 1209:0001
# Bus 002 Device 018: ID 1209:0001 Generic pid.codes Test PID
```

## 3. Build

```sh
cd examples/host-rs
cargo build --release
```

The binary is at `target/release/iotsploit-host`. There are **no external
crate dependencies**, so the build works offline.

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

## 6. Command reference

General form:

```
iotsploit-host [--device <path>] <command> [args]
```

`--device /dev/usbtmc0` selects a specific node when several are present.

| Command | What it does |
|---|---|
| `list` | Print every `/dev/usbtmc*` node (does not open the device). |
| `idn` | Query `*IDN?` and print the identity string. |
| `caps` | Query `SYSTem:CAPabilities?`, parse it, print structured fields. |
| `headers` | Fetch and list all command headers (`SYSTem:HELP:HEADers?`). |
| `query '<cmd>'` | Send any SCPI query, print its text response. |
| `write '<cmd>'` | Send a non-query SCPI command (no response printed). |
| `block-read '<cmd>' [--out <file>]` | Query a binary-block response and write the payload to a file (or stdout). |
| `errors` | Drain the `SYSTem:ERRor?` queue until "No error". |
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
profile/workflow engine is a later milestone, so for now drive it by hand with
raw SCPI. From the REPL (or as separate one-liners):

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

As one-liners (e.g. inside a shell loop):

```sh
sudo iotsploit-host write  'BLE:SCAN:CLEar'
sudo iotsploit-host write  'BLE:SCAN:START'
# poll until state reports 0
while [ "$(sudo iotsploit-host query 'BLE:SCAN:STATe?')" = "1" ]; do sleep 0.5; done
n=$(sudo iotsploit-host query 'BLE:SCAN:COUNt?')
for i in $(seq 0 $((n-1))); do
  sudo iotsploit-host query "BLE:SCAN:RESult? $i"
done
```

## 9. How it works

```
your shell ──► iotsploit-host (Rust) ──► /dev/usbtmc0 (kernel USBTMC driver)
                                                  │
                                                  ▼
                                         USB cable (USBTMC)
                                                  │
                                                  ▼
                                      iotsploit-usb firmware
                                      (libscpi + TinyUSB)
```

- **Transport** (`transport.rs`): an abstract `write_msg` / `read_msg` trait.
  The only backend today is `usbtmc_kernel.rs`, which lets the kernel handle
  USBTMC framing — exactly like the original Python host.
- **Session** (`session.rs`): appends the SCPI `\n` terminator, trims trailing
  CR/LF from text, and never decodes binary blocks as text.
- **Block** (`block.rs`): parses/encodes IEEE 488.2 definite-length arbitrary
  blocks (`#<digits><len><payload>`), preserving arbitrary bytes.
- **Caps** (`caps.rs`): tolerantly parses `SYSTem:CAPabilities?`.
- **Headers** (`headers.rs`): lists commands; falls back to paging for large
  command sets.

The whole crate is dependency-free on purpose, so it builds anywhere a Rust
toolchain exists.

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
cargo test
```

Hardware smoke test (nRF52840, after granting access):

```sh
iotsploit-host idn
iotsploit-host caps
iotsploit-host headers
iotsploit-host query 'BLE:SCAN:STATe?'
iotsploit-host block-read 'DATA:READ? 64' --out /tmp/adc.bin
iotsploit-host errors
```
