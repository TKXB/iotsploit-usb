# Simple Commands and Workflow

Status: decided, not started. Proposal page with current-vs-proposed
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

Workflows, 12 → 9; a name means the same behaviour and result fields on every
board:

| Today | Proposed |
|---|---|
| `ble-scan` (esp32s3, nrf52840; different fetch and fields) | `ble-scan` |
| `ble-connect`, `ble-pair`, `ble-connect-pair`, `ble-auto` | `ble-connect` (picks from last scan or scans first; pairs unless `--no-pair`; `--name` filter; reports security) |
| `ble-peripheral` (nrf52840), `ble-security` (esp32s3-spp-security) | `ble-peripheral` |
| `wifi-scan`, `ble-sniff`, `demo-scan` | unchanged names, job pattern |

Open question: the three CAN boards have no workflow; decide whether
`can-capture` adds anything beyond `stream`.

Firmware, 3 steps: declare each command once (X-macro emits both the libscpi
table and the descriptor), `tools/fw <board> flash`, `tools/fw <board> check`.

## Findings this fixes

- Default build lacks `tcp`, yet `--help` shows TCP and `stream` examples.
- Device errors print an empty line and exit 0.
- Handler and descriptor tables drift: nRF52840 has 31 handlers vs 26
  descriptor entries (3 used by its own `ble-scan` workflow, 2 stubs);
  Pico2 has no descriptor.
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
9. **nRF52840 descriptor drift.** Describe `BLE:SCAN`, `BLE:SCAN:DONE?`,
   `BLE:SCAN?` (used by its `ble-scan` workflow); delete the
   `BLE:PAIR:CONFirm` and `BLE:PAIR:NUMCmp?` stubs, which no prompt uses.
10. **Wrong summary.** esp32s3 `ble-connect`: it connects, it does not pair.
11. **Docs.** Move the ESP32-S3 build/flash section out of the root README into
    `examples/esp32s3/README.md` with generic paths; fix host README
    mismatches (`describe` output, workflow output, test count) and document
    the new commands.
12. **Phase gate.** C gate, `cargo test`, `tests/scpi_tcp_smoke.py`, and the
    per-OS smoke checklist on every available board.

### Phase 2: One vocabulary (old names kept as aliases)

1. **Decide the open question**: does CAN get a `can-capture` workflow or is
   `stream` enough.
2. **Command standard.** `.agents/standards/scpi-commands.md`: the three
   patterns, the state words (`IDLE RUNNING DONE FAILED CONNECTING CONNECTED
   PAIRING PASSKEY CONFIRM DISPLAY ADVERTISING`), one parameter dictionary
   (`pin value index duration channel key accept bus id data bitrate filter`),
   and `fields=` required on every `FETCh?`.
3. **One command list.** `USBSCPI_DEFINE_COMMANDS` X-macro and `P_*` param
   macros in `include/usbscpi/usbscpi.h`, emitting the libscpi table and the
   descriptor table. Test in `tests/test_usbscpi.c` that both tables hold the
   same headers.
4. **Workflow macros.** `USBSCPI_WF_ACQUIRE(name, prefix, summary, fields)`
   derives `:STARt`, `:STATe?`=`DONE`, `:COUNt?`, `:FETCh?`;
   `USBSCPI_WF_INTERACTIVE(...)` takes named states and prompts. No host
   engine change: it already compares trimmed strings.
5. **Workflow renames in the descriptor.** Add a `deprecated=<new-name>` key so
   the host can print "renamed to …" for one release.
6. **Migrate boards, one commit each**, in this order: `daemon` (covered by
   the TCP smoke test), `esp32s3`, `nrf52840`, `butterfly-nrf52840`,
   `esp32s3-spp-security`, `stm32f4disco`, `stm32f4disco-socketcan`, `can`,
   `pico2`. Each: X-macro list, three patterns, named states, dictionary
   parameter names, old headers as hidden aliases (in the libscpi table, not
   the descriptor), `:STATus?` removed (reasons go to `SYST:ERR?`).
7. **Consolidate workflows** to the nine in the table above, on the
   boards that have them.
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
