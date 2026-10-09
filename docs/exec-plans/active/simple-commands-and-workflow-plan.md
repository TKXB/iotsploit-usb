# Simple Commands and Workflow

Status: phase 1 done; phases 2 and 3 not started. Proposal page with current-vs-proposed
comparisons: https://claude.ai/artifact/NxmUuLzzxvQ7KXArH3qZbj

## Goal

Commands and workflow must be simple and easy to use. The rule for every
decision: if a new user cannot guess it, it does not ship. The device's
descriptor is the single source of truth; the host, help, validation and checks
are all derived from it.

## Target surface

Host CLI, 8 commands:

| Command | Purpose | Replaces |
|---|---|---|
| `devices` | Every reachable device on every transport, numbered, with IDN | `list` |
| `info` | IDN, transport, caps, command/workflow counts | `idn`, `caps` |
| `help [name]` | All commands and workflows; with a name, its params and summary | `headers`, `describe`, `profile` |
| `run [workflow] [--param v]` | No name lists workflows; named params; table output | `workflow` |
| `send <cmd> [name=value…]` | Raw SCPI or typed args; detects query/write/block | `query`, `write`, `block-read` |
| `stream [count]` | Data plane | unchanged |
| `shell` | REPL with history and completion | `repl` |
| `check` | Access/driver setup, then descriptor, naming and serial conformance | new |

Global options: `-d <n|uri>` (`usbtmc:0`, `usb:vid:pid[/serial]`,
`tcp://host[:port]`, or `IOTSPLOIT_DEVICE`), `--format text|json|csv`,
`--timeout <ms>`. Removed: `--backend`, `--vid`, `--pid`, `--serial`.

Errors: every command reads `SYST:ERR?` and reports device errors inline,
suggesting the closest header. Exit codes: 0 ok, 1 device error, 2 usage,
3 connect. `errors` is removed.

Device commands, three patterns on every board (all 141 described commands
fit one):

- **Setting**: write with a value, read the same node with `?`
  (`GPIO 2,1` / `GPIO? 2`, `LED red,1` / `LED? red`, `BUTTon?`). Read-only
  values (`BLE:SEC?`, `SYST:STR:PORT?`) are settings you can only read.
- **Action**: does one thing now (`CAN:SEND`, `BLE:INJect`,
  `BLE:DISConnect`); failures go to `SYST:ERR?` as readable messages.
- **Job**: anything that runs over time uses `:STARt`, `:STOP`, `:STATe?`
  (a word: `IDLE|RUNNING|DONE|FAILED|PASSKEY|…`), `:COUNt?`, `:FETCh? <n>`,
  `:CLEar`.

The same parameter has the same name and type on every board. The three
`:STATus?` queries (raw GAP/SMP codes) are removed in favour of the error queue.
Old names stay as aliases for one release.

Workflows, 12 → 10 (nine, plus the new `can-capture`); a name means the same
behaviour and result fields on every board:

| Today | Proposed |
|---|---|
| `ble-scan` (esp32s3, nrf52840; different fetch and fields) | `ble-scan` |
| `ble-connect`, `ble-pair`, `ble-connect-pair`, `ble-auto` | `ble-connect` (picks from last scan or scans first; pairs unless `--no-pair`; `--name` filter; reports security) |
| `ble-peripheral` (nrf52840), `ble-security` (esp32s3-spp-security) | `ble-peripheral` |
| `wifi-scan`, `ble-sniff`, `demo-scan` | unchanged names, job pattern |
| none (CAN boards) | `can-capture` |

Decided: the CAN boards get a `can-capture` workflow (12 → 10 workflows in
total). `stream` stays for continuous capture over TCP; `can-capture` is the
bounded, works-on-every-transport version:

```text
CAN:CAPTure:STARt <duration>[,<bus>]   clear, then capture for <duration> s;
                                       bus omitted = every open bus
CAN:CAPTure:STATe?                     IDLE | RUNNING | DONE | FAILED
CAN:CAPTure:COUNt?                     frames captured
CAN:CAPTure:FETCh? <n>                 time_us,bus,id,ext,rtr,len,data
CAN:CAPTure:STOP / :CLEar
```

Fields `time:u64:us,bus:u32,id:hex,ext:bool,rtr:bool,len:u32,data:hex`,
the same on `can`, `stm32f4disco` and `stm32f4disco-socketcan`. A full capture
buffer ends the window early with `DONE`.

Firmware, 3 steps: declare each command once (X-macro emits both the libscpi
table and the descriptor), `tools/fw <board> flash`, `tools/fw <board> check`.

## Findings this fixes

- Default build lacks `tcp`, yet `--help` shows TCP and `stream` examples.
- Device errors print an empty line and exit 0.
- Handler and descriptor tables drift: nRF52840 has 31 handlers vs 26
  descriptor entries (2 used by its own `ble-scan` workflow, 1 UI alias,
  2 UI stubs); Pico2 has no descriptor.
- Ten state queries return bare numbers that differ by board: "pair done" is
  `4` on esp32s3 and `3` on nrf52840; `ble-connect-pair` succeeds on `6` vs
  `5`.
- Same parameter, different name or type: GPIO `value` vs `level`; passkey
  string vs u32; scan length `duration` vs `secs` vs none; `CAN:OPEN`/`SEND`
  differ between the Linux and STM32 boards; `CAN:STATe?` has three layouts.
- `esp32s3` `ble-connect` summary says it pairs; it only connects.
- stm32f4disco boards: 10 of 21 commands are per-colour LED duplicates.
- Every board reports USB serial `0001`; identical boards are
  indistinguishable.
- Start has three spellings (`START`, `STARt`, bare); result fetch has four
  (`RESult?`, `<SUB>?`, `PACKet?`, `RECV?`); nRF52840 ships two scan APIs.
- Pairing states are bare numbers that differ between `BLE:PAIR` and
  `BLE:CPAIR`.
- Seven examples redefine the same seven TinyUSB USBTMC callbacks.
- Four flashing tools; main README carries personal paths.

## Phases

Each step is one commit. Every step that touches C, headers, build files or
transport tests passes `tools/testing/test-c-full.sh`; every host step passes
`cargo test` (all feature combinations). Board steps are verified on the board.

### Phase 1: Fixes (nothing breaks; old commands keep working)

1. **Default TCP.** `host/rust/Cargo.toml`: `default = ["kernel", "tcp"]`.
   Done when `--device tcp://127.0.0.1:5025 idn` works against the daemon in a
   default build.
2. **Exit codes.** `iotsploit-host.rs` `main()`: 0 ok, 1 device error,
   2 usage, 3 connect (failures from `open_backend`). Done when tests cover
   each code.
3. **Automatic error check.** After every `write`/`query` from the CLI, read
   `SYST:ERR?` until "No error" and print `error <code> <message>`, plus the
   closest header from `HEADers?` on -113. Not inside workflow polling loops
   (the trigger check already exists). Done when a mistyped header exits 1
   with a message.
4. **`send`.** One command: `?` → query, `#<digit>` reply → binary block,
   otherwise write; `-o <file>` anywhere. `query`, `write`, `block-read` stay
   as aliases that print a one-line hint.
5. **`info` and `help [name]`.** `info` = IDN + transport + caps + counts.
   `help` = descriptor commands and workflows, falling back to `HEADers?`
   when there is no descriptor; `help <name>` = params, types, summary.
   `idn`, `caps`, `headers`, `describe`, `profile` become hinting aliases.
6. **`devices` and `-d`.** List kernel nodes and raw USB devices (when
   compiled), open each, show IDN, number them. `-d` takes a number,
   `usbtmc:N`, `usb:VID:PID[/serial]` or `tcp://host[:port]`; read
   `IOTSPLOIT_DEVICE` when `-d` is absent. TCP devices are not discoverable,
   so they appear only when named. `--backend/--vid/--pid/--serial` and
   `list` become hinting aliases.
7. **Unique serials.** Each board fills USB string 3 and the IDN serial field
   from the chip ID (ESP32-S3 eFuse MAC, nRF52840 FICR DEVICEID, RP2040
   `pico_get_unique_board_id`, STM32 `UID_BASE`). Done when two identical
   boards show different serials in `devices`.
8. **Pico2 descriptor.** Describe `GPIO:SET`, `GPIO:GET?`, `ADC:READ?`.
9. **nRF52840 descriptor drift.** Describe `BLE:SCAN` and `BLE:SCAN:DONE?`,
   the trigger and done query of its own `ble-scan` workflow. `BLE:SCAN?` and
   the `BLE:PAIR:CONFirm`/`BLE:PAIR:NUMCmp?` stubs stay undescribed and are
   kept: the iotsploit-ui BLE panel calls them.
10. **Wrong summary.** esp32s3 `ble-connect`: it connects, it does not pair.
11. **Docs.** Move the ESP32-S3 build/flash section out of the root README into
    `examples/esp32s3/README.md` with generic paths; fix host README
    mismatches (`describe` output, workflow output, test count) and document
    the new commands.
12. **Phase gate.** C gate, `cargo test`, `tests/scpi_tcp_smoke.py`, and the
    per-OS smoke checklist on every available board.

**Phase 2 note (nRF52840 ble-auto).** `ble-connect` is identical on the
ESP32-S3 and nRF52840 (`BLE:CONNect:STARt <index>[,<pair>]`), so a host drives
both the same way. The nRF's old `BLE:AUTO` (scan + name-filter + connect +
pair) stays as a working, undescribed alias: driving "scan first, then connect
by name" from one workflow needs a host-side name->index step the engine does
not have yet, and `--name` is deferred rather than faked. `ble-connect` lists
`ble-auto` and `ble-connect-pair` as its `renamed_from`.

**Phase 2 note (CAN capture).** `can-capture` lands on the two STM32 boards
as a job (`CAN:CAPTure:STARt/STOP/STATe?/COUNt?/FETCh?/CLEar`) over a snapshot
drained from the existing RX ring in SCPI-task context, so there is no new ISR
state; it is STOP-bounded because the boards have no millisecond clock, and its
fetch row is `bus,id,ext,rtr,len,data` (the board keeps no timestamp). The Linux
`can` board stays stream-only: it already captures continuously over its TCP
data plane (`stream`), which carries `ts_us`, so it gets no `CAN:CAPTure` job —
the same board that has a stream and no scan job on the ESP32-S3 side. Net
workflows: 10 (nine BLE/Wi-Fi/sniff/demo plus `can-capture`). The STM32
`can-capture` path is built here but not flashed; the stm32 LED per-colour and
`LED:ALL` demo headers collapse into `LED <index>,<value>` per the standard,
with `LED:SET`/`LED:GET?`/`BTN?`/`CAN:RECV?`/`CAN:COUNt?` kept as aliases.

**Phase 1 outcome.**

- Host: steps 1–6 as written. `send` checks errors in the same message
  (`*CLS;<cmd>;:SYSTem:ERRor?`): verified on the daemon that a failed query
  then returns its error immediately instead of timing out over TCP, and that
  USBTMC's single-message replies and TCP's per-line replies parse alike.
- Firmware: steps 7–10 as written, plus three fixes the Pico2 example needed to
  build at all (glue include path, stale TinyUSB descriptor macros, a comment
  ended early by `msgBulkIn_*/`); its `io_buf` grew to 1024 bytes to hold the
  descriptor.
- Built here: stm32f4disco, stm32f4disco-socketcan, pico2. Not built (SDK
  downloads blocked): nrf52840, butterfly-nrf52840, esp32s3,
  esp32s3-spp-security; their descriptor tables were compiled and emitted
  through the core on the host, and parsed by `iotsploit-host help`.
- Not flashed or exercised on hardware.

### Phase 2: One vocabulary (old names kept as aliases)

1. **Decided**: CAN gets `can-capture` (see the workflow table above).
2. **Command standard.** `.agents/standards/scpi-commands.md`: the three
   patterns, the job states (`IDLE RUNNING DONE FAILED`, plus `PASSKEY CONFIRM
   DISPLAY` while a job waits on the user), one parameter dictionary
   (`pin value index duration channel key accept bus id data bitrate filter`),
   and `fields=` required on every `FETCh?`.
3. **One command list.** `USBSCPI_DEFINE_COMMANDS` X-macro and `P_*` param
   macros in `include/usbscpi/usbscpi.h`, emitting the libscpi table and the
   descriptor table. Test in `tests/test_usbscpi.c` that both tables hold the
   same headers.
4. **Workflow macros.** `USBSCPI_WF_ACQUIRE(name, prefix, summary, fields)`
   derives `:STARt`, `:STATe?`=`DONE`, `:COUNt?`, `:FETCh?`;
   `USBSCPI_WF_INTERACTIVE(...)` takes named states and prompts. The host
   engine already compares trimmed strings; it gains one change: a fetch
   workflow stops on `FAILED` instead of waiting for the timeout.
5. **Workflow renames in the descriptor.** A workflow lists its old names
   (`renamed_from`); the core emits the full WF line again under each old name
   with `renamed=<new-name>`. Older hosts ignore the key and keep working; the
   new host prints "renamed to …".
6. **Migrate boards, one commit each**, in this order: `daemon` (covered by
   the TCP smoke test), `esp32s3`, `nrf52840`, `butterfly-nrf52840`,
   `esp32s3-spp-security`, `stm32f4disco`, `stm32f4disco-socketcan`, `can`,
   `pico2`. Each: X-macro list, three patterns, named states, dictionary
   parameter names, old headers as hidden aliases (in the libscpi table, not
   the descriptor), `:STATus?` removed (reasons go to `SYST:ERR?`).
7. **Consolidate workflows** to the ten in the table above (nine plus
   `can-capture`), on the boards that have them.
8. **`run`.** No name lists workflows; named params (`--duration 8`) from the
   descriptor; progress on stderr; Ctrl-C sends the job's `:STOP`; results as
   a table from `fields=`; an index param with an options source shows a
   picker (scans first if there are no results).
9. **`--format text|json|csv`** on every command that prints data.
10. **Phase gate.** As phase 1, plus every workflow run on its board, and
    old names still accepted.

### Phase 3: Tooling and cleanup

1. **Typed `send`.** `send GPIO pin=2 value=1`: arguments checked against
   descriptor types and required flags before anything is sent.
2. **Fallback descriptor in the core.** When `cfg.descriptor` is NULL,
   `SYSTem:HELP:DESCription?` emits a minimal descriptor from the registered
   headers.
3. **TinyUSB defaults in the glue.** Move the seven required USBTMC callbacks
   into `glue/usbscpi_tinyusb.c` (opt-out macro for custom capabilities);
   delete the seven copies in `examples/`.
4. **Shared USB descriptors.** A glue helper plus a board serial hook; delete
   the five `usb_descriptors.c` copies, so a board supplies only VID/PID and
   product string.
5. **`check`.** Host setup (node permissions, udev rule, WinUSB binding), then
   the device: descriptor matches `HEADers?`, names follow the standard, serial
   is unique, every workflow's commands exist.
6. **`tools/fw <board> build|flash|monitor|check`.** A thin wrapper around
   `idf.py`, the nRF Makefile + `nrfjprog`, `make flash` (OpenOCD) and the
   Pico SDK/UF2; `check` runs `iotsploit-host check`.
7. **`shell`.** History and completion from the descriptor. Decision point:
   hand-written line editing keeps zero dependencies; a crate does not.
8. **Remove aliases**: old SCPI headers, old CLI commands and flags, and the
   `deprecated=` workflow names.
9. **Release binaries.** The repo has no CI today; add one that builds and
   tests the host for Linux, Windows and macOS and publishes the four named
   binaries.
10. **Phase gate.** As phase 2, plus `tools/fw <board> check` passing on
    every available board.

## Constraints

Core stays C99, stack-neutral and allocation-free. Host stays dependency-free.
Raw SCPI remains available through `send` and `shell`. Each phase runs
`tools/testing/test-c-full.sh` and `cargo test` before commit; hardware
behaviour is verified on boards, not inferred from host tests.
