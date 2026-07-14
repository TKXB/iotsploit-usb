# Plan: consolidate duplicated example code into the iotsploit-usb core/glue

## Status and relationship to the existing plan

This supersedes and sharpens [`refactor-examples-to-core.md`](refactor-examples-to-core.md),
which was written against three examples (`pico2`, `esp32s3`, `nrf52840`). Three
more have since landed and confirm every duplication that plan predicted:

- `examples/stm32f4disco/` (libopencm3 + TinyUSB DWC2)
- `examples/butterfly-nrf52840/` (BLE sniff/inject dialect)
- `examples/esp32s3-spp-security/` (BLE advertise + pairing-only dialect)

The Tier 1→3 framing from the older doc still holds. This document is the
concrete, step-by-step version: exact files, symbols, signatures, CMake wiring,
acceptance criteria, and sequencing.

## Evidence base (measured, not assumed)

| Duplication | Where it appears today |
|---|---|
| 8 required USBTMC control stubs | all 6 example entry points, ~50 lines each; the 7 trivial ones are byte-identical across pico2/stm32/nrf52840/butterfly |
| `usb_tx` forwarding thunk | every TinyUSB example (`pico2`, `stm32f4disco`, `nrf52840`, `butterfly`, both esp32s3) |
| `usb_descriptors.c` | 82–87 lines, near-identical; two byte-equivalent spellings (hand-rolled vs `TUD_USBTMC_*` macros) |
| Triple-declared commands | handler + `scpi_command_t[]` + `usbscpi_command_desc_t[]`, e.g. stm32 has 16×3 |
| BLE SCPI surface | 4 boards, 4 diverging dialects (see Move 5) |

Facts that constrain the plan:

- No portable weak-symbol macro exists yet in `src/`, `glue/`, or `include/`.
- The glue is an opt-in static lib gated by `USBSCPI_BUILD_TINYUSB_GLUE`
  (default `OFF`); `usbscpi_tinyusb.c` is also compiled directly into the
  `test_glue_tinyusb` host test against `tests/stub/tusb.h`.
- `usbscpi_tinyusb_tx()` already has the exact `usbscpi_usb_tx_t` signature.
- `usbscpi_task()` is currently a no-op; 5 of 6 examples call it, `pico2` does
  not.
- The core is built with `-Wall -Wextra -Werror`; the glue is not (it depends on
  TinyUSB headers not present in the host build), so anything moved into the
  glue must not be added to the `-Werror` core target.

## Guiding constraint

`glue/usbscpi_tinyusb.c` must remain the **only** owner of the USBTMC data-path
callbacks (`tud_usbtmc_msg_data_cb`, `tud_usbtmc_msgBulkIn_request_cb`,
`tud_usbtmc_msgBulkIn_complete_cb`, `tud_usbtmc_get_stb_cb`, the two
clear-feature callbacks). None of the moves below touch that path, so the
`-110` regression fixed by `test_glue_tinyusb` stays fixed. Every move is
gated behind "still links + `test_glue_tinyusb` still passes."

---

## Move 1 — USBTMC "device defaults" unit in the glue *(do first)*

**Goal:** delete the 8 copy-pasted required-callback stubs from all 6 apps.

### 1.1 Add a portable weak macro

New header `glue/usbscpi_weak.h`:

```c
#ifndef USBSCPI_WEAK_H
#define USBSCPI_WEAK_H

#if defined(_MSC_VER)
#  define USBSCPI_WEAK        /* MSVC: no weak; host build never links these */
#elif defined(__GNUC__) || defined(__clang__)
#  define USBSCPI_WEAK __attribute__((weak))
#else
#  define USBSCPI_WEAK
#endif

#endif
```

Weak (not strong) so any board that needs non-default capabilities/abort
behavior can still override by defining the symbol normally — no `#ifdef` dance.

### 1.2 Add the defaults translation unit

New file `glue/usbscpi_tinyusb_device.c` providing `USBSCPI_WEAK` definitions
of the eight symbols currently pasted into every app:

- `tud_usbtmc_get_capabilities_cb` — both the `CFG_TUD_USBTMC_ENABLE_488` and
  the non-488 variant, exactly as in `examples/pico2/main.c:73-93`.
- `tud_usbtmc_msgBulkOut_start_cb` → `return true`
- `tud_usbtmc_initiate_abort_bulk_out_cb` / `check_abort_bulk_out_cb`
- `tud_usbtmc_initiate_abort_bulk_in_cb` / `check_abort_bulk_in_cb`
- `tud_usbtmc_initiate_clear_cb` / `check_clear_cb`

All the "trivial seven" just set `USBTMC_STATUS_SUCCESS` and `return true`; copy
the verified block from `examples/pico2/main.c:95-122` verbatim.

Declare nothing new in a public header — these are TinyUSB strong symbols with
fixed prototypes; the app just stops defining them.

### 1.3 CMake

Add `glue/usbscpi_tinyusb_device.c` to the `usbscpi_tinyusb_glue` target and to
the `test_glue_tinyusb` sources. The host stub `tests/stub/tusb.h` must gain the
minimal `usbtmc_response_capabilities*` / `usbtmc_*_rsp_t` types the new TU uses;
keep them behind the same stub the glue test already relies on.

### 1.4 Delete from apps

Remove the stub block (and its explanatory comment) from:
`examples/pico2/main.c`, `examples/stm32f4disco/main.c`,
`examples/nrf52840/main.c`, `examples/butterfly-nrf52840/main.c`,
`examples/esp32s3/main/app_main.c`,
`examples/esp32s3-spp-security/main/app_main.c`.
Each app now links the `_device` unit instead.

### Acceptance criteria

- All 6 examples link with the stub block removed.
- Any board that had non-default capabilities keeps them by defining the one
  symbol locally (weak override), with no `#ifdef`.
- `test_glue_tinyusb` and `test_usbscpi` still pass.
- Core `-Werror` target unchanged (new code lives only in the glue/test targets).

---

## Move 2 — Retire the `usb_tx` thunk

**Goal:** remove the identical 4-line `usb_tx` from every TinyUSB app.

Each app writes:

```c
static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    (void)user; (void)eom;
    return usbscpi_tinyusb_tx(NULL, data, len, true);
}
```

Two options; pick one and apply everywhere:

- **A (minimal):** document that apps set `cfg.usb_tx = usbscpi_tinyusb_tx`
  directly (signatures already match). Zero new code.
- **B (encapsulated):** have `usbscpi_tinyusb_bind()` set `cfg.usb_tx` itself, so
  the app leaves `cfg.usb_tx` NULL. Requires `bind()` to run before/around init,
  or a new `usbscpi_tinyusb_attach(cfg)` that fills the field pre-init.

**Decision to resolve before coding:** the thunk forces `eom=true` and ignores
the core's `eom`. Confirm whether that is deliberate USBTMC message framing. If
yes, option A changes behavior (core would pass real `eom`), so option B — with
`bind()` forcing `eom=true` internally — is the faithful choice. If the forced
`true` was incidental, option A is fine. **Do not silently change the wire
behavior**; verify against a real device query round-trip.

### Acceptance criteria

- No example defines a `usb_tx` thunk.
- A hardware (or `test_glue_tinyusb`) round-trip shows query replies still
  arrive with correct framing (no `-110`, no truncation).

---

## Move 3 — Parameterized USB descriptors + shared tusb_config fragment

**Goal:** one descriptor generator driven by an ident struct; delete four
near-identical `usb_descriptors.c`.

### 3.1 Ident struct + generator

New `glue/usbscpi_tinyusb_descriptors.{c,h}`:

```c
typedef struct {
    uint16_t vid, pid, bcd_device;
    const char *manufacturer, *product, *serial;
} usbscpi_usb_ident_t;

void usbscpi_tinyusb_descriptors_init(const usbscpi_usb_ident_t *ident);
```

The unit provides `tud_descriptor_device_cb`, `tud_descriptor_configuration_cb`,
and `tud_descriptor_string_cb` for a single USBTMC interface with two bulk
endpoints, built from TinyUSB's `TUD_USBTMC_*` macros (the spelling already used
by stm32/nrf52840/butterfly). Endpoint addresses and the FS/HS max packet size
stay `#define`-overridable for boards that need different EPs.

### 3.2 Prove byte-equivalence

`pico2` hand-rolls the interface/endpoint bytes; the macro form must produce the
**same** bytes. Add a host-side static check (a small test that compares the
generated configuration descriptor against the frozen pico2 byte array) rather
than assuming equality. This is the one place the older plan explicitly warns
about; keep that guard.

### 3.3 Shared tusb_config fragment

Add `glue/tusb_config_usbtmc.h` with the USBTMC-common defines
(`CFG_TUD_USBTMC`, buffer sizes, `CFG_TUD_USBTMC_ENABLE_488`, endpoint sizes).
Each board's `tusb_config.h` includes it and keeps only MCU/PHY/RHPORT
selection, which is genuinely board-specific.

### 3.4 Delete from apps

Remove `usb_descriptors.c` from `pico2`, `stm32f4disco`, `nrf52840`,
`butterfly-nrf52840` (esp32s3 variants use the managed TinyUSB descriptor path —
handle separately, do not force them onto this generator in this move). Each app
calls `usbscpi_tinyusb_descriptors_init(&ident)` at startup.

### Acceptance criteria

- Generated config descriptor is byte-identical to the current pico2 array
  (static check passes).
- The 4 boards enumerate as USBTMC and `*IDN?` returns the string built from the
  ident struct.
- No behavioral change in VID/PID (`0x1209`/`0x0001`) unless intentionally
  parameterized per board.

---

## Move 4 — Collapse the command triple-declaration

**Goal:** declare each command once; make the `scpi_command_t[]` and
`usbscpi_command_desc_t[]` tables impossible to disagree.

### 4.1 Add the register helper (public API)

```c
int usbscpi_register_with_descriptor(
    usbscpi_t *ctx,
    const scpi_command_t *commands,
    const usbscpi_descriptor_t *descriptor);
```

Plus a debug-build assertion that every `scpi_command_t.pattern` has a matching
`usbscpi_command_desc_t.pattern` and vice versa (linear scan at init; it is
one-time and small). This alone catches drift at boot.

### 4.2 Optional X-macro for single-source tables

For one **stable, small** surface first — GPIO/LED, not BLE — express the
command list once:

```c
#define GPIO_COMMANDS(X) \
  X("GPIO:SET",  cmd_gpio_set, 0, "command", "Set pin output level", gpio_set_params, 2, "none") \
  X("GPIO:GET?", cmd_gpio_get, 0, "query",   "Read pin input level", gpio_get_params, 1, "u32")
```

One expansion emits `scpi_command_t[]`, another emits
`usbscpi_command_desc_t[]`. Prove the shape on `stm32f4disco` (its LED/GPIO
surface is the cleanest) before touching anything else.

### Acceptance criteria

- `usbscpi_register_with_descriptor()` exists and the assert fires on a
  deliberately mismatched test table.
- stm32f4disco's LED/GPIO tables are generated from a single list; its
  `SYSTem:HELP:DESCription?` output is unchanged (diff the emitted block before
  and after).

---

## Move 5 — Shared BLE feature module behind a backend vtable *(last; needs a contract first)*

This is where real logic — not plumbing — is duplicated, and it is **already
drifting across four boards**:

| Board | BLE dialect (measured) |
|---|---|
| `nrf52840` | timed `BLE:SCAN` **plus** native `BLE:SCAN:START/STOP/RESult/STATe/CLEar`, `BLE:AUTO`, full `BLE:PAIR/CPAIR` |
| `esp32s3` | timed `BLE:SCAN`/`SCAN:DONE?`/`SCAN?` only, `BLE:PAIR/CPAIR` |
| `esp32s3-spp-security` | `BLE:ADV:STARt/STOP`, pairing-only, no scan |
| `butterfly-nrf52840` | `BLE:SNIFf*`, `BLE:CHANnel`, `BLE:INJect` |

The `BLE:PAIR:*` / `BLE:CPAIR:*` state enums and workflow success/fail values
differ between esp32s3 and nrf52840 for what is meant to be one host-facing
contract. The Rust host and iotsploit-ui depend on this vocabulary being
uniform, so this is the highest-consequence item — and the one that cannot be
mechanically extracted.

### 5.1 Write the contract down first (no code)

Decide and document, before any extraction:

- Canonical scan dialect (timed vs native start/stop vs both).
- Which compatibility aliases survive, and whether they appear in
  `SYSTem:HELP:DESCription?` or stay hidden for old clients.
- Exact state enum values for `BLE:PAIR:STATe?`, `BLE:CPAIR:STATe?`, `BLE:AUTO`.
- Workflow `success_value` / `failed_values` hosts may depend on.
- Whether sniff/inject (`butterfly`) and advertise (`spp-security`) are separate
  optional feature modules or part of one BLE surface.

### 5.2 Then define the surface once

`feature/ble/ble_scpi.c` owns the SCPI command block + descriptor + workflow
metadata, calling a board-supplied vtable:

```c
typedef struct {
    int    (*scan_start)(uint16_t secs);
    bool   (*scan_done)(void);
    size_t (*scan_count)(void);
    int    (*scan_get)(size_t i, char *buf, size_t len);
    int    (*conn_start)(size_t i);
    int    (*pair_start)(void);
    /* ...pairing confirm / passkey / state accessors... */
} ble_backend_t;
```

nRF (SoftDevice) and ESP32 (NimBLE) implement only the backend. Sniff/advertise
become separate optional modules with their own vtables so a scanner board does
not pull in inject code.

### Acceptance criteria

- The canonical BLE contract is a written doc reviewed against current Rust-host
  and iotsploit-ui expectations.
- `esp32s3` and `nrf52840` emit **identical** `SYSTem:HELP:DESCription?` BLE
  records (diff them).
- Each board compiles with only its backend implemented.

---

## What must NOT move (correctly board-specific)

Clock/PHY/power bringup, IRQ vectors (`otg_fs_isr`, `USBD_IRQHandler`), the
GPIO/ADC HAL, `tusb_config.h` MCU/RHPORT selection, and the main-loop shape
(esp32s3's FreeRTOS task split + delayed wireless init vs pico2's bare
`while(tud_task())`). A generic `main()` stays out of scope.

## Loose end to standardize along the way

`usbscpi_task()` is a no-op today but 5 of 6 examples call it and `pico2` does
not. Whatever move touches the app loops should make all apps call it uniformly,
so `pico2` does not silently break when `usbscpi_task()` gains a body.

## Sequencing

1. **Move 1** — glue device-defaults unit + weak macro; delete the 8 stubs.
   Mechanical, fully verifiable by link + `test_glue_tinyusb`.
2. **Move 2** — retire the `usb_tx` thunk (resolve the `eom=true` question
   first).
3. **Move 3** — parameterized descriptors + tusb_config fragment, guarded by the
   pico2 byte-equivalence check.
4. **Move 4** — `usbscpi_register_with_descriptor()` + assert, then the X-macro
   on stm32 LED/GPIO only.
5. **Define the BLE contract** (doc), then **Move 5** — shared BLE module behind
   a vtable.
6. Add a `templates/new_device/` skeleton last, once the ident struct, command
   helper, and feature vtable seams are settled.

Each step lands and is verified (link + existing tests + a hardware query
round-trip) before the next begins. Nothing in steps 1–4 touches the USBTMC
data path or the BLE contract.
