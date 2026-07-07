# Refactoring: move common firmware code from `examples/` into the core

## Goal

Let a user create a new device firmware **easily and in a standard way**: a new
device should be one small file plus a hardware backend, with all USB/USBTMC
plumbing and the host-facing contract owned by the component instead of
copy-pasted per board.

## What is being copy-pasted today

Comparing the three example entry points (`examples/pico2/main.c`,
`examples/esp32s3/main/app_main.c`, `examples/nrf52840/main.c`) against `src/`,
`glue/`, and the descriptor tables, the duplication falls into three concentric
tiers.

### Tier 1 — USB/USBTMC plumbing (mostly identical, little board variance)

- `glue/usbscpi_tinyusb.c` already owns the TinyUSB USBTMC data path:
  `tud_usbtmc_open_cb`, `tud_usbtmc_msg_data_cb`,
  `tud_usbtmc_msgBulkIn_request_cb`, `tud_usbtmc_msgBulkIn_complete_cb`,
  `tud_usbtmc_get_stb_cb`, and the bulk IN/OUT clear-feature callbacks. That
  ownership should remain there; new code must not duplicate these strong
  symbols.
- The remaining required TinyUSB USBTMC callbacks are still copy-pasted in each
  app: `tud_usbtmc_get_capabilities_cb`,
  `tud_usbtmc_msgBulkOut_start_cb`, and the abort/clear control request stubs.
  They are trivial and identical, but every app currently has to know they
  exist or linking fails.
- The USB descriptors are conceptually the same: device descriptor, one USBTMC
  interface, two bulk endpoints, and string descriptors. pico2 and esp32s3
  hand-roll the interface/endpoint bytes; nrf52840 uses TinyUSB's USBTMC
  descriptor macros. A shared implementation should preserve or prove
  byte-equivalence rather than assuming all descriptor spellings are identical.
- The `usb_tx` thunk, the `s_storage` / `s_line` / `s_io` buffers, and the
  `usbscpi_config_t` block are near-identical, but buffer sizing and
  descriptors differ by feature set.

### Tier 2 — SCPI command thunks

Dozens of 3–5 line functions that all follow one of two shapes: "parse uint32 →
call backend → `SCPI_Result*`" or "`SCPI_Result*(backend_state())`". The BLE
surface alone is ~25 of these per board.

### Tier 3 — descriptor/command bookkeeping

Every command is declared **three times**: once as a handler, once in the
`scpi_command_t[]` table, once in the `usbscpi_command_desc_t[]` table. Nothing
enforces they agree, and they have already drifted — esp32s3 and nrf52840 have
*different* `BLE:PAIR:STATe?` / `BLE:CPAIR:STATe?` state enums and different
workflow success values for what is meant to be the same host-facing contract.

### What is genuinely board-specific (correctly not shared)

Clock/power/PHY bringup, IRQ vectors (`USBD_IRQHandler`), the GPIO/ADC HAL, the
RTOS loop shape, and `tusb_config.h` MCU selection.

## The core problem

A new-device author today must reproduce all of Tier 1 and Tier 2 correctly —
including the subtle USBTMC descriptor constants and the "define these 8 or the
link fails" trap — before writing a single line of their own feature. And the
Tier 3 triple-declaration means the host-facing contract silently rots. The
README documents these traps in prose, which is a tell that they should be code,
not documentation.

## Recommended approach: extract plumbing first, then tighten the contract

The target is that a new device is **one small file plus a backend
implementation**, with the plumbing owned by the component. Three moves, in
priority order.

### Move 1 — Add a TinyUSB USBTMC "device defaults" unit to the glue tier

*(highest value, lowest risk)*

Add `glue/tinyusb/usbscpi_tinyusb_device.c` (+ header) that owns the Tier 1
pieces not already owned by `glue/usbscpi_tinyusb.c`:

- Default implementations for the remaining required USBTMC callbacks, marked
  with a portable `USBSCPI_WEAK` macro so an app can still override them.
- A parameterized descriptor set driven by one struct the app fills in. The
  descriptor generator should use TinyUSB's USBTMC descriptor macros when they
  are available, and fall back to explicit interface/endpoint descriptors only
  where needed:

  ```c
  usbscpi_usb_ident_t ident = {
      .vid = 0x1209, .pid = 0x0001,
      .manufacturer = "IoTSploit", .product = "nRF52840 USBTMC", .serial = "0001",
  };
  usbscpi_tinyusb_device_init(&ident);
  ```

- A shared `tusb_config_usbtmc.h` fragment the ports include.

This deletes the link-trap stubs and most descriptor boilerplate without
touching the already-working IN/OUT glue. Board bringup (PHY/IRQ/clock), task
shape, and interrupt vectors stay in each app for now; that variance is real.

Acceptance criteria:

- `usbscpi_tinyusb.c` remains the only owner of the USBTMC data-path callbacks.
- pico2, esp32s3, and nrf52840 all link with the copied stubs removed.
- Descriptor bytes are either covered by a host-side test/static check or
  verified on hardware with the existing host tools.
- Existing `test_glue_tinyusb` coverage still passes, because Move 1 must not
  regress the IN/OUT sequencing fixed there.

### Move 2 — Collapse the triple-declaration with a table macro

Declare each command once and generate both tables from it, so they cannot
drift:

```c
#define BLE_COMMANDS(X) \
  X("BLE:SCAN",        cmd_ble_scan,  "command", "Start BLE scan for N seconds", p_secs, "none") \
  X("BLE:SCAN:STATe?", cmd_ble_state, "query",   "1=scanning 0=idle",            NULL,   "bool") \
  /* ... */
```

One expansion emits the `scpi_command_t[]`, another emits the
`usbscpi_command_desc_t[]`. At minimum, add a
`usbscpi_register_with_descriptor()` helper plus a compile-time length assert so
the two tables cannot disagree. This kills the entire Tier 3 class of bugs.

Do this first on one narrow command family, not the whole tree at once. The
first target should be a stable, small surface such as GPIO/ADC or Wi-Fi scan;
BLE should wait until its contract is reconciled in Move 3.

### Move 3 — Promote BLE to a shared "feature module" behind a backend vtable

This is where the *real* logic (not plumbing) is duplicated and already
drifting. Before writing the shared module, decide the canonical host-facing
BLE contract:

- Which scan dialect is canonical: timed `BLE:SCAN` / `BLE:SCAN:DONE?` /
  `BLE:SCAN?`, native start/stop/result commands, or both.
- Which compatibility aliases remain, and whether they are advertised in
  `SYSTem:HELP:DESCription?` or kept hidden for old host/UI clients.
- The exact state enum values for `BLE:PAIR:STATe?`, `BLE:CPAIR:STATe?`, and
  any automatic workflow.
- The workflow success and failure values hosts should depend on.

After that contract is explicit, define the BLE SCPI command block, descriptor,
and workflow metadata **once** in `feature/ble/ble_scpi.c`, calling a
board-supplied vtable:

```c
typedef struct {
    int    (*scan_start)(uint16_t secs);
    bool   (*scan_done)(void);
    size_t (*scan_count)(void);
    int    (*scan_get)(size_t i, char *buf, size_t len);
    int    (*conn_start)(size_t i);
    int    (*pair_start)(void);
    /* ... */
} ble_backend_t;
```

nRF (SoftDevice) and ESP32 (NimBLE) each implement only the backend. The
host-facing SCPI vocabulary, state enums, and descriptor are then *guaranteed
identical* across boards — which is the property the iotsploit-ui and Rust host
actually depend on.

## Target layout

```text
core/            stack-neutral SCPI (today's src/)
glue/tinyusb/    USBTMC defaults: descriptors + required control callbacks
glue/            existing USBTMC IN/OUT glue (today's usbscpi_tinyusb.c)
port/            optional later: port_esp32s3.c, port_pico2.c, port_nrf52840.c
feature/ble/     shared BLE SCPI surface + descriptor, calls ble_backend_t
apps/<device>/   IDN + feature selection + board choice — minimal
templates/new_device/   fill-in-the-blank skeleton (+ optional tools/new_device.py scaffolder)
```

Do not move `main()` into core as part of Move 1. esp32s3 has a FreeRTOS task
split and delayed wireless init; nrf52840 has SoftDevice, USB power events, and
`USBD_IRQHandler` ordering. A tiny `usbscpi_port.h` (`usbscpi_port_init()`,
`usbscpi_port_task()`) can be introduced later if the extracted defaults prove
stable, but generic `main()` should be treated as a separate follow-up.

## Suggested sequencing

1. **Move 1 first** — extract TinyUSB descriptor defaults and the missing
   required control callbacks only. Verify links/tests before deleting example
   code.
2. **Move 2 on a small surface** — add the command/descriptor consistency
   mechanism and apply it to one stable command family. This proves the shape
   without entangling BLE behavior.
3. **Define the BLE contract** — reconcile esp32s3/nrf52840 command names,
   aliases, state values, workflow success/failure values, and descriptor
   visibility.
4. **Move 3** — promote BLE to a shared feature module behind a backend vtable
   once the contract is written down.
5. Add the `templates/new_device/` skeleton last, once the seams (ident struct,
   command metadata helper, feature vtable, and any optional port interface) are
   settled.

Start with Move 1, but keep it deliberately narrow: descriptor/default-callback
extraction, no generic `main()`, no data-path callback churn, and no BLE
contract changes.
