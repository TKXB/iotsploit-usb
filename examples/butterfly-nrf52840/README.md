# butterfly nRF52840 — BLE sniffer over USBTMC/SCPI

Multi-protocol-RF firmware in the spirit of
[butterfly](https://github.com/whad-team/butterfly), driven over the
`iotsploit-usb` core (USBTMC transport + SCPI command path) instead of
butterfly's native USB CDC-ACM + WHAD protobuf transport. This first version
implements a **timed BLE advertising-channel sniffer** (plus raw PDU inject),
modeled as a bounded `trigger_poll_fetch` workflow so the existing
descriptor-driven `iotsploit-host` and Flutter UI drive it with **zero host-side
changes**.

See [`docs/butterfly-usbtmc-integration-plan.md`](../../docs/butterfly-usbtmc-integration-plan.md)
for the full design rationale.

## What this is (and the butterfly-source note)

The integration plan keeps butterfly's **radio core** and drops its
transport/WHAD layers. butterfly's upstream source is not vendored into this
repo, so `radio_ble.c` is a **self-contained, faithful reimplementation of the
BLE advertising-channel RX/TX path** on the nRF52840 `RADIO` peripheral — the
same bare-metal, no-SoftDevice radio-ownership model butterfly uses. The other
butterfly protocols (802.15.4, ESB, Unifying, Mosart, ANT) would plug in here as
additional `RADIO` configurations behind the same SCPI/descriptor pattern.

This follows **Path A** from the plan: TinyUSB's USBTMC class driver +
`glue/usbscpi_tinyusb.c`, reused unchanged. There is **no SoftDevice** — the
firmware owns the radio directly, which is why the original SoftDevice-based nRF
demo is not reused.

## Tech Stack

| Layer | Library | Role |
|---|---|---|
| RF radio | `radio_ble.c` (this example) | Bare-metal nRF52840 `RADIO` BLE advertising RX/TX (1 Mbit/s, AA 0x8E89BED6, BLE whitening + CRC) |
| Capture buffer | `ble_sniff_handler.c` (this example) | Timed window, bounded static ring, dropped counter, CSV/hex row formatting |
| SCPI parser | libscpi v2.3 (vendored) | SCPI-99 parsing, error queue, parameter handling |
| USBTMC core | iotsploit-usb (`src/usbscpi.c`) | Stack-neutral USBTMC receive/respond + descriptor emission — **unchanged** |
| USB device stack | TinyUSB | USB device framework with USBTMC class + Nordic `dcd_nrf5x` driver |
| SoC drivers | nRF5 SDK `nrfx_clock` / `nrfx_power` | HFCLK + USB VBUS power events (no SoftDevice) |
| SCPI-over-USB glue | `glue/usbscpi_tinyusb.c` | Bridges TinyUSB USBTMC callbacks to the iotsploit-usb core — **reused as-is** |

### Architecture

```
main.c                     usbscpi_tinyusb.c          usbscpi.c
(SCPI command table,       (TinyUSB USBTMC glue:      (SCPI core: parse,
 descriptor, USB/radio      bulk-IN/OUT routing)        respond, SYST:HELP:DESC?)
 bring-up)                        |                          |
   |          |                   v                          v
   |          |            TinyUSB dcd_nrf5x            libscpi v2.3
   v          v            (USBD device driver)
ble_sniff_    radio_ble.c
handler.c     (nRF RADIO peripheral:
(ring +        BLE adv RX/TX, bare-metal)
 formatting)
```

## Hardware

| Feature | Detail |
|---|---|
| MCU | nRF52840 (Cortex-M4F, 64 MHz) |
| Boards | Nordic PCA10059 dongle, Makerdiary MDK dongle, PCA10056 DK |
| Radio | 2.4 GHz `RADIO`, BLE 1 Mbit/s, adv channels 37/38/39 |
| USB | Full-speed device peripheral (USBTMC) |
| Clock | HFXO crystal (required by `RADIO`) started in `radio_ble_init()` |
| Flash | 1 MB at 0x00000000 — **application at 0x0 (no SoftDevice)** |
| SRAM | 256 KB at 0x20000000 |

## SCPI interface

| Command | Kind | Description |
|---|---|---|
| `BLE:SNIFf [<secs>[,<channel>]]` | command | Start a timed advertising-capture window (default 5 s; optional channel 37/38/39) |
| `BLE:SNIFf:STOP` | command | Stop the capture window early |
| `BLE:SNIFf:DONE?` | query | `1` = window finished, `0` = still capturing |
| `BLE:SNIFf:COUNt?` | query | Number of buffered PDUs |
| `BLE:SNIFf:PACKet? <i>` | query | Record `i` as a text row (see below) |
| `BLE:SNIFf:DROPped?` | query | PDUs dropped because the ring was full |
| `BLE:CHANnel <n>` / `BLE:CHANnel?` | command / query | Select / read the advertising channel |
| `BLE:INJect <hex>` | command | Transmit a raw advertising PDU given as a hex string |

### Captured-row format

`BLE:SNIFf:PACKet? <i>` returns one CSV text row (advertised via the workflow's
`fields=` schema, so the UI renders a table automatically):

```
<timestamp_us>,<channel>,<rssi_dbm>,<length>,<pdu_hex>
```

`pdu_hex` is the whole advertising PDU (2-byte header + payload) as lowercase
hex. `length` is that PDU byte count.

### Workflow

The descriptor advertises one `trigger_poll_fetch` workflow, `ble-sniff`:
trigger `BLE:SNIFf` → poll `BLE:SNIFf:DONE?` until `1` → `BLE:SNIFf:COUNt?` →
`BLE:SNIFf:PACKet? <i>` for each record. This is the exact pattern the host's
`run_trigger_poll_fetch` already runs for the ESP32 Wi-Fi/BLE scans.

### Design limits (from the zero-host-edit constraint)

- **Batch capture windows**, not continuous streaming: capture N seconds, fetch,
  repeat.
- Bounded static ring (`MAX_SNIFF_RESULTS = 64`), one query round-trip per
  fetched PDU. Overflow is counted via `BLE:SNIFf:DROPped?`, not buffered.
- Only CRC-valid PDUs are stored.
- Live streaming / binary packet records / PCAP export are explicitly deferred
  (they require host + UI edits).

## Build

Requires the ARM GCC toolchain, the nRF5 SDK 17.1.0, and TinyUSB.

```sh
make SDK_ROOT=$HOME/nordic/nRF5_SDK_17.1.0_ddde560 \
     TINYUSB_ROOT=$HOME/nordic/tinyusb
```

(Both default to `$HOME/nordic/...`, so a bare `make` works with that layout.)

Output: `_build/nrf52840_xxaa.{out,hex,bin}`. The app links at flash `0x0` (no
SoftDevice), so no SoftDevice hex is merged.

## Flash (not part of the build step)

- **PCA10056 DK / J-Link:** `nrfjprog --program _build/nrf52840_xxaa.hex --chiperase --reset`
- **PCA10059 / Makerdiary dongle (USB DFU):** convert to UF2/DFU and program with
  `nrf-uf2` or `nrfutil` in the Open Bootloader — because the app sits at `0x0`,
  use a build variant offset for the Nordic bootloader if you keep it.

> This example is validated to **build only**; it is intentionally not flashed
> here. Radio-timing behaviour under the TinyUSB ISR (the plan's primary Phase 0
> risk) must be validated on hardware before relying on capture completeness.
