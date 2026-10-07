# ESP32-S3 EvilDuck integration — implementation status

Issue: TKX-209. Plan: `esp32s3-evilduck-integration.md`.
Implemented: 2026-10-07. Branch: `tkx-209-esp32s3-evilduck`.

Milestones 1 and 2 are implemented and validated on real hardware. Milestone 3
(filesystem persistence) is intentionally deferred — the plan marks it optional
("only if persistent scripts are needed"). Nothing is committed yet; see
"Before committing" below.

## What was built

A new, distinct firmware example `examples/esp32s3-hid/` (composite USBTMC + HID
keyboard), plus a portable, host-tested core shared by firmware and the C gate:

| File | Role |
| --- | --- |
| `main/duck_keymap.{h,c}` | Explicit US-layout ASCII/token → HID usage + modifier table |
| `main/duckscript.{h,c}` | Bounded line classifier + whole-script validator (REM, STRING, DELAY, chords) |
| `main/duck_runner.{h,c}` | Asynchronous, bounded, cancellable runner; injected HID/clock/ready |
| `main/duck_store.{h,c}` | Single-owner upload: committed + staging slots, validate-on-commit |
| `main/app_main.c` | USB PHY, composite descriptors, HID backend, `DUCK:*` SCPI, runner pump |
| `main/tusb_config.h` | USBTMC + HID on; vendor dropped to fit the IN-endpoint budget |
| `components/iotsploit-usb/CMakeLists.txt` | ESP-IDF shim: core + TinyUSB glue only (no socket/stream, no lwip) |
| `tests/test_duckscript.c` | Host test (registered in CTest as `test_duckscript`) |

SCPI contract (also in the `SYSTem:HELP:DESCription?` descriptor and the
`duck-run` zero-prompt workflow): `DUCK:RUN`, `DUCK:STOP`, `DUCK:STATe?`,
`DUCK:LINE?`, `DUCK:ERR?`, `DUCK:UPLoad:STARt <len>`, `DUCK:UPLoad:ABORt`,
`DUCK:UPLoad:STATe?`, and the core-intercepted `DATA:WRITE` block into staging.
States: 0 idle, 1 running, 2 done, 3 error, 4 cancelled. No script runs at boot
or on reconnect; a USB detach ends an active run with a delivery error and
requires a new RUN (no resume). The runner is polled from the USB service task
and never blocks (bounded work; returns on a busy endpoint or unmet deadline).

## Test evidence

- C gate (`tools/testing/test-c-full.sh`): **7/7 passed, 0 failed, 0 skips**,
  including the new `test_duckscript` (keymap, validation, runner press/release
  ordering, cancel-during-delay, runtime budget, busy backpressure, delivery
  failure, detach, reject-second-run, upload store, end-to-end). Built under
  `-Wall -Wextra -Werror`.
- Firmware: `idf.py build` (ESP-IDF v5.2.2), binary 0x47ab0, 81% app partition free.
- Hardware (board MAC `34:85:18:41:C6:AC`, flashed over `/dev/ttyUSB0`):
  - Enumerates as `1209:0002 "IoTSploit ESP32-S3 USBTMC+HID"`, exposing
    `/dev/usbtmc3` and a HID keyboard (`event21`).
  - `DUCK:RUN` of the fixed demo → `STATe?` 0→2, `LINE?` 3; captured keystrokes
    `iotsploit esp32s3-hid <Shift>HID demo<ENTER>` (uppercase via Shift correct).
  - Upload via `DATA:WRITE` → `UPLoad:STATe? ready`; committed run typed the
    uploaded text.
  - STOP during a 1.5 s DELAY → `STATe? 4`, nothing typed (responsive cancel).
  - Invalid upload → `UPLoad:STATe? error`, committed slot preserved (next RUN
    still ran the last-good script).
- Checked on Linux only. Windows enumeration/typing is untested.

## Deferred / not done

- Milestone 3: ESP-IDF filesystem persistence, LIST/named RUN, language growth
  (DEFAULT_DELAY, bounded REPEAT, variables, locales).
- HID-only + TCP build profile (milestone 1 step 4). This build is composite,
  SCPI over USBTMC only; no Wi-Fi/BLE pulled in.
- Open design question from the review: in composite mode the USB-side USBTMC
  interface can drive `DUCK:*`. Decide and document whether the target host may
  control the runner, and fold it into the auth gap. The raw SCPI listener does
  not authenticate.

## Before committing (per `.agents/standards/testing.md`)

- This is a firmware feature change, so the built-in firmware fuzz gate
  (`tools/hardware/firmware_fuzz_gate.py`) must be run with a `duck`/HID feature
  suite before a feature commit. Not yet run.
- Enable the repo hook once: `git config core.hooksPath tools/git-hooks`.
- Move this plan to `completed/` only after the deferred items above are either
  done or explicitly descoped and the fuzz gate passes.
