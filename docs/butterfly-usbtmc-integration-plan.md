# Butterfly-over-USBTMC Integration Plan (2026-07-06)

## Implementation status (2026-07-06)

First version implemented in `examples/butterfly-nrf52840/` and building clean
(`make` → `_build/nrf52840_xxaa.{hex,bin}`, no SoftDevice). It follows **Path A**
(TinyUSB USBTMC + `glue/usbscpi_tinyusb.c`, reused as-is) and delivers the BLE
advertising sniffer end-to-end: bare-metal nRF `RADIO` core (`radio_ble.c`),
timed capture ring + text-row formatting (`ble_sniff_handler.c`), the `BLE:SNIFf`
`trigger_poll_fetch` workflow, and a well-formed `SYSTem:HELP:DESCription?`
descriptor — with **no host/UI edits**. butterfly's upstream source is not
vendored, so `radio_ble.c` is a faithful reimplementation of the BLE
advertising RX/TX path rather than a literal fork; the other protocols plug in
behind the same pattern. Validated to build only — not flashed; the Phase 0
radio-timing smoke test still needs hardware.

## Summary

Integrate the [butterfly](https://github.com/whad-team/butterfly) multi-protocol
RF firmware into `iotsploit-usb` so that an `iotsploit-ui` / SCPI-USBTMC host can
drive it. The integration keeps **butterfly's radio core and per-protocol
controllers** and replaces butterfly's native transport (USB CDC-ACM + the WHAD
protobuf protocol) with **`iotsploit-usb`'s core function: USBTMC + SCPI**.

**Scope constraints (decided):**

- Ignore the existing `examples/nrf52840` SoftDevice BLE demo entirely. It is not
  reused, and its SoftDevice-based scan/connect/pair path is dropped on this
  firmware.
- Use **only** `iotsploit-usb`'s core: USBTMC transport + SCPI command path
  (`src/usbscpi.c` + vendored libscpi). No vendor bulk-IN endpoint — everything
  rides the one USBTMC pipe.
- WHAD-on-the-wire is **not** preserved. The device is driven by SCPI, so
  `whad-client` / pywhad compatibility is intentionally given up. (If WHAD
  compatibility later becomes a hard requirement, see
  [Alternative: WHAD tunnel](#alternative-whad-tunnel-not-chosen).)

## Background: why this is a fork, not a merge

The two projects make opposite choices at every layer, so integration means
picking which of butterfly's layers to keep and rebuilding the rest.

| Layer          | butterfly                                   | iotsploit-usb                          |
| -------------- | ------------------------------------------- | -------------------------------------- |
| Radio          | bare-metal, direct radio (`radio.cpp`)      | (nRF demo used Nordic SoftDevice)      |
| RF protocols   | BLE / 802.15.4 / ESB / Unifying / Mosart / ANT via per-protocol controllers | n/a (demo only) |
| USB stack      | Nordic nRF5-SDK USBD (`app_usbd_cdc_acm`)   | TinyUSB (nRF demo), stack-neutral core |
| USB class      | CDC-ACM serial                              | USBTMC (USB488)                        |
| Wire protocol  | WHAD (protobuf/nanopb, preamble+len+CRC)    | SCPI text + definite-length binary blocks |
| Host driver    | WHAD Python client                          | SCPI/USBTMC host (Rust `iotsploit-host`) |

**Radio ownership** is the reason the nRF demo cannot be reused as-is: butterfly
owns the raw radio bare-metal, the demo used the SoftDevice, and both cannot own
the radio at once. Because we drop the demo, this conflict simply disappears —
the new firmware gives the radio to butterfly.

**What we keep from butterfly:** `core`, `controller`, `radio*`, `packet`,
`timer`, and the `controllers/` (blecontroller, dot15d4controller,
esbcontroller, etc.).

**What we remove from butterfly:** `serial.cpp` and the `whad-lib` submodule /
WHAD protobuf dispatch — replaced by the `iotsploit-usb` SCPI command path.

## Design constraint: zero host-side changes

**Requirement: the host and UI must not be edited.** The existing `iotsploit-usb`
host is fully **descriptor-driven** — `host/rust/src/workflow.rs`'s
`run_trigger_poll_fetch` reads a workflow from `SYSTem:HELP:DESCription?` and runs
trigger → poll `done` → query `count` → fetch each result *as text*, and the
descriptor's `fields=` / `result_fields=` column schema drives the Flutter table
renderer (`usbtmc_descriptor.dart`, `usbtmc_control_panel.dart`,
`usbtmc_result.dart`). The existing **timed BLE scan** already rides this exact
path with no device-specific host code.

Therefore, if butterfly capture is modeled the same way the BLE scan is, **all
host and UI code is reused unchanged** and the entire integration lives in the
firmware. The rules to stay on that path:

1. **Model capture as a bounded `trigger_poll_fetch` workflow, not a live
   stream:** `BLE:SNIFf <secs>` → poll `BLE:SNIFf:DONE?` → `BLE:SNIFf:COUNt?` →
   `BLE:SNIFf:PACKet? <i>`. Capture into a window, then fetch the buffered
   records.
2. **Return each packet as a text row** (hex-encoded CSV), *not* a binary block,
   with a `fields=` schema in the descriptor. The generic fetch path handles text;
   this also avoids any `src/usbscpi.c` binary-block edit.
3. **Everything — commands, descriptor, capture buffer — lives in the firmware.**

### What this design costs

- **Batch capture windows, not continuous live streaming.** Capture for N
  seconds, fetch the buffer, repeat for the next window.
- **One query round-trip per packet**, bounded by a static firmware result ring
  (like `MAX_SCAN_RESULTS`). Fine for tens/low-hundreds of packets; poor for
  high-rate sniffing (thousands/sec).
- **Text rows capped at the line-record MTU** — large PDUs must fit one row.
- **No PCAP export or live packet table** — those are host features and are out of
  scope while host code is frozen.

### If you later need real streaming (opt-in, breaks the constraint)

Continuous streaming, high packet rates, binary packet records, or PCAP export
**require host + UI edits** and are explicitly deferred. The upgrade path is
polled batched binary-block drain queries (`BLE:SNIFf:DRAin?` returning
`SCPI_ResultArbitraryBlock`), optionally hinted by the USB488 status byte — but
that needs a new host capture runner and packet decoder, so it is not part of the
first version. Keep v1 SCPI-text + `trigger_poll_fetch` only.

## The decision that dominates firmware effort: USB stack

`iotsploit-usb`'s core is stack-neutral, but it does **not** implement the USBTMC
*class* itself — it relies on the USB stack's USBTMC driver for bulk endpoints and
USBTMC bulk headers. **TinyUSB ships a USBTMC class driver; the Nordic nRF5-SDK
USBD does not** (it provides CDC-ACM, MSC, HID, …). Butterfly is built on nRF5-SDK
USBD. This forces a choice, and it is the fork point for the whole effort:

- **Path A — port butterfly's USB to TinyUSB.** Reuse `glue/usbscpi_tinyusb.c` and
  TinyUSB's USBTMC driver unchanged (TinyUSB supports the nRF52840 device
  peripheral). *Least new USB code.* **Risk:** butterfly's sniffing is
  timing-sensitive and was tuned around the SDK USBD interrupt behavior — must
  validate no packet drops under TinyUSB's ISR latency.

- **Path B — implement USBTMC on nRF5-SDK USBD.** Keep butterfly's existing USB
  init / UF2 DFU bootloader and radio timing; write a USBTMC class driver on
  `app_usbd` plus a new glue that feeds `src/usbscpi.c`. *More USB-class work*,
  but lower radio-timing risk.

**Trade-off:** Path A minimizes new code but risks radio timing; Path B preserves
butterfly's proven timing but costs a USBTMC-class implementation. Resolve this in
Phase 0 with a spike + a radio-drop smoke test.

## Phased plan

### Phase 0 — Spike the USB-stack choice
- Bring up `iotsploit-usb`'s core on the nRF52840 dongle far enough to answer
  `*IDN?` over USBTMC, via **Path A** and/or **Path B**.
- Run a radio-timing smoke test (BLE sniff a known advertiser) under the chosen
  USB stack; confirm packets are not dropped while USB is active.
- **Decision gate:** pick Path A or Path B based on effort + timing result.

### Phase 1 — Butterfly core with transport removed
- Fork butterfly. Keep `core` / `controller` / `radio*` / `packet` / `timer` /
  `controllers/`.
- Remove `serial.cpp` and the `whad-lib` submodule / WHAD protobuf dispatch.
- Feed received USBTMC bytes into `usbscpi.c`; wire the `usb_tx` callback back out
  through the chosen USB stack.

### Phase 2 — SCPI control plane
- Register an `scpi_command_t` table mapping to butterfly controllers, e.g.:
  - `BLE:SNIFf <secs>` (timed capture trigger), `BLE:SNIFf:STOP`
  - `BLE:CHANnel <n>`
  - `BLE:INJect <hex>` — take the PDU as a **hex-text parameter**, not a binary
    block, so no `src/usbscpi.c` edit is needed. (A binary-block inject path would
    require either the `DATA:WRITE` selector convention or a core parser
    extension; deferred with the streaming upgrade.)
  - later: `DOT15D4:*`, `ESB:*`, `UNIFying:*`
- Implement `SYSTem:HELP:DESCription?` returning the line-record descriptor
  (`CMD` / `WF` / `DEV` tags), including the capture `WF` and its `fields=` column
  schema, so the existing descriptor-aware host + UI auto-render everything with
  no host/UI code.

### Phase 3 — SCPI data plane (text-row `trigger_poll_fetch`)
- Capture into a bounded static result ring during the timed window (populated
  from the `blecontroller` RX callback), mirroring the BLE-scan result store.
- Workflow queries, matching the existing scan pattern:
  - `BLE:SNIFf:DONE?` — poll for window complete.
  - `BLE:SNIFf:COUNt?` — number of buffered records.
  - `BLE:SNIFf:PACKet? <i>` — return record `i` as a **text row** (hex-encoded
    CSV), sized to fit the line-record MTU.
- Define overflow behavior: ring capacity (like `MAX_SCAN_RESULTS`), and a
  dropped-packet counter exposed via `BLE:SNIFf:DROPped?`.
- Record fields (as CSV columns, advertised by `fields=`): timestamp, channel,
  RSSI, length, PDU hex.

### Phase 4 — Host / UI (no changes)
- **No host or UI edits.** `host/rust/src/workflow.rs`'s `run_trigger_poll_fetch`
  and the Flutter descriptor renderer already drive this workflow from the
  descriptor and render the fetched text rows via the `fields=` schema — exactly
  as they do for the existing timed BLE scan.
- The only requirement is that the firmware descriptor is well-formed; verify by
  running the existing `iotsploit-host` against the new firmware unmodified.

### Phase 5 — Build & flash
- Merge butterfly's nRF5-SDK Makefile build with `iotsploit-usb`'s core (and, for
  Path A, the TinyUSB sources).
- Produce a UF2 / hex artifact; document the flashing procedure for the
  Makerdiary MDK and Nordic PCA10059 dongles.

**Prove the whole idea end-to-end with BLE sniff only** (timed capture window →
fetch text rows) against the **unmodified** host before adding other protocols.

## Files touched

**All edits are in firmware; the host and UI are reused unchanged** (see
[Phase 4](#phase-4--host--ui-no-changes)). `N` = new file, `E` = edit existing,
`R` = reuse unchanged. Paths are relative to their repo root. The exact
firmware-target location is a proposal — `examples/butterfly-nrf52840/` mirrors
the existing example layout.

### Firmware — `third_party/iotsploit-usb/`

- `N` `examples/butterfly-nrf52840/` — new target: forked butterfly core
  (`core`, `controller`, `radio*`, `packet`, `timer`, `controllers/`) with the
  WHAD/serial transport removed. Contains:
  - `N` `main.c` — init, SCPI command-table registration, `usb_tx` callback,
    USBTMC/USB glue wiring (mirrors `examples/nrf52840/main.c`).
  - `N` `ble_sniff_handler.{c,h}` — wraps butterfly's `blecontroller`; owns the
    capture result ring, dropped-packet counter, and text-row (CSV/hex) record
    formatting for `BLE:SNIFf:PACKet?`.
  - `N` `usb_descriptors.c`, `tusb_config.h` — Path A (TinyUSB USBTMC), adapted
    from `examples/nrf52840/`.
  - `N` `Makefile`, `linker.ld`, `sdk_config.h` — merge butterfly's nRF5-SDK
    build with iotsploit-usb's core (+ TinyUSB for Path A).
- Path A: `R` `glue/usbscpi_tinyusb.c`, `glue/usbscpi_tinyusb.h` — reused as-is.
- Path B: `N` `glue/usbscpi_nrf5_usbd.c`, `.h` — new USBTMC class driver +
  glue on `app_usbd`, feeding `src/usbscpi.c`.
- `R` `src/usbscpi.c`, `include/usbscpi/usbscpi.h` — **unchanged.** The text-row +
  hex-text-inject design needs no core parser change (no command-specific binary
  blocks).
- `E` `.gitmodules` / vendoring — drop butterfly's `whad-lib` submodule; `serial.cpp`
  is not carried into the fork.

### Host + UI — reused unchanged

No edits. The design deliberately fits the existing descriptor-driven
`trigger_poll_fetch` path so these are all `R` (reuse):

- `R` `host/rust/src/{descriptor,workflow,session}.rs` — parse the descriptor and
  run the capture workflow as-is.
- `R` outer `ui/rust/src/api/usbtmc.rs` + `lib/rust/**` (FRB bridge) — no new API.
- `R` `lib/screens/utils/usbtmc/{usbtmc_descriptor,usbtmc_control_panel,usbtmc_result}.dart`
  — auto-render the control commands and the fetched packet table from the
  descriptor's `fields=` schema.

## Risks

- **Radio timing under a swapped/updated USB stack** (Phase 0) — the primary
  technical risk. Validate packet-drop behavior before building out.
- **Capture throughput / capacity** — bounded static ring + one round-trip per
  fetched packet means only bursts of tens–hundreds of packets per window; a
  timed window can overflow the ring at high packet rates (surfaced via
  `BLE:SNIFf:DROPped?`). This is the price of the zero-host-edit constraint.
- **Descriptor must be well-formed** — since there is no host code to adapt, the
  workflow only works if the firmware's `SYSTem:HELP:DESCription?` exactly matches
  the `trigger_poll_fetch` grammar (incl. `fields=`). Verify against the
  unmodified `iotsploit-host`.
- **Given up:** WHAD-client compatibility; SoftDevice-based BLE central operations
  (scan/connect/pair) from the old demo; and — under the zero-host-edit constraint
  — live streaming, binary packet records, and PCAP export.
- **Effort:** multi-week firmware effort, not a wiring change. The USB-stack fork
  (Path A vs B) is the largest single unknown.

## Alternative: WHAD tunnel (not chosen)

If WHAD-client compatibility becomes a hard requirement, keep butterfly's WHAD
protobuf `Message` frames intact and tunnel them as raw USBTMC binary blocks
(DEV_DEP_MSG in/out) instead of translating to SCPI. This preserves the WHAD wire
semantics and the ability to point pywhad at the device (through a small
USBTMC↔WHAD shim), but the descriptor-driven auto-UI does not apply — the host
needs a bespoke butterfly panel that encodes/decodes protobuf. Documented here for
completeness; the SCPI-native plan above is the chosen path.
