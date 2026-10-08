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

Device commands, 6 verbs on every subsystem:
`<SUB>:STARt`, `:STOP`, `:STATe?` (a word: `IDLE|RUNNING|DONE|FAILED|…`),
`:COUNt?`, `:FETCh? <n>`, `:CLEar`. Old names stay as aliases for one release.

Firmware, 3 steps: declare each command once (X-macro emits both the libscpi
table and the descriptor), `tools/fw <board> flash`, `tools/fw <board> check`.

## Findings this fixes

- Default build lacks `tcp`, yet `--help` shows TCP and `stream` examples.
- Device errors print an empty line and exit 0.
- Handler and descriptor tables drift: nRF52840 has 31 handlers vs 26
  descriptor entries; Pico2 has no descriptor.
- Every board reports USB serial `0001`; identical boards are
  indistinguishable.
- Start has three spellings (`START`, `STARt`, bare); result fetch has four
  (`RESult?`, `<SUB>?`, `PACKet?`, `RECV?`); nRF52840 ships two scan APIs.
- Pairing states are bare numbers that differ between `BLE:PAIR` and
  `BLE:CPAIR`.
- Seven examples redefine the same seven TinyUSB USBTMC callbacks.
- Four flashing tools; main README carries personal paths.

## Phases

1. **Fixes.** Default `tcp` feature; automatic error check and exit codes;
   `devices`, `info`, `help`, `send` (raw); unique serial from chip ID;
   Pico2 descriptor; nRF52840 drift; README path cleanup.
2. **One vocabulary.** Command style guide in `.agents/standards/`; aliases;
   named states; X-macro command list and workflow macros; `run` with named
   params, tables and `--format`.
3. **Tooling.** Typed `send` args; `shell` completion; `check`; default
   TinyUSB callbacks and USB descriptors in `glue/`; core-generated fallback
   descriptor; `tools/fw`; remove aliases.

## Constraints

Core stays C99, stack-neutral and allocation-free. Host stays dependency-free.
Raw SCPI remains available through `send` and `shell`. Each phase runs
`tools/testing/test-c-full.sh` and `cargo test` before commit; hardware
behaviour is verified on boards, not inferred from host tests.
