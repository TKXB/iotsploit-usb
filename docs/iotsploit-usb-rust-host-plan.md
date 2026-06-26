# iotsploit-usb Generic Rust Host Plan

## Summary

Build a generic Rust host for `iotsploit-usb` that can control devices through SCPI over USBTMC without hardcoding every board-specific workflow in Rust code.

The recommended target is:

1. A stable Rust core that speaks SCPI over `/dev/usbtmcN`.
2. Runtime discovery using existing device commands such as `*IDN?`, `SYSTem:CAPabilities?`, and `SYSTem:HELP:HEADers?`.
3. A machine-readable optional descriptor command that exposes parameters, return types, and higher-level workflow metadata.
4. A profile fallback for devices that only expose command headers but not a full descriptor yet.

This gives a practical migration path from the current Python host script to one host binary that can control multiple `iotsploit-usb` devices.

## Implementation Status

Tracked on branch `feat/host-rs` (work uncommitted, ready for review).

| Milestone | Scope | Status |
|---|---|---|
| 0 | Planning & skeleton | DONE |
| 1 | Linux `/dev/usbtmc` transport | DONE |
| 2 | SCPI session & text queries | DONE |
| 3 | Binary block support | DONE |
| 4 | Profile metadata (TOML) | TODO |
| 5 | Workflow engine | TODO |
| 6 | Descriptor client (Rust) | TODO |
| 7 | Firmware descriptor (`SYST:HELP:DESC?`) | TODO |
| 8 | Optional raw USB backend (`rusb`) | TODO (optional) |

### Completed (Milestones 0-3 = First PR scope)

Implemented under `examples/host-rs/` as a dependency-free crate:

- `src/lib.rs`, `transport.rs`, `usbtmc_kernel.rs`, `block.rs`, `session.rs`,
  `caps.rs`, `headers.rs`, `src/bin/iotsploit-host.rs`
- CLI: `list`, `idn`, `caps`, `headers`, `query`, `write`, `block-read`,
  `errors`, `repl`, plus `--device`, `--help`, `--version`
- 32 unit + fake-transport tests pass (`cargo test`); `cargo build --release`
  is clean with **no external dependencies**.

Verified on hardware (nRF52840, VID `1209:0001`, `/dev/usbtmc0`):

- `idn` -> `IoTSploit,nRF52840,0001,0.1.0`
- `caps` -> tolerates the nRF libc `%zu`->`zu` quirk (mtu/maxblock reported
  as `None` with a warning instead of failing)
- `headers` -> all 17 registered commands
- `query 'BLE:SCAN:STATe?'` -> `0`
- `block-read 'DATA:READ? 64' --out /tmp/adc.bin` -> 0-byte file (empty `#10`
  block; this board wires no data source to `DATA:READ?`)
- `write 'BLE:SCAN:CLEar'` -> returns in ~21 ms with no hang; follow-up
  `*OPC?`/`*IDN?` confirm no read desync
- `errors` -> `(no errors)`

Protocol note (confirmed from libscpi source, not just probing): non-query
commands produce no SCPI output, but the TinyUSB glue answers every kernel IN
request with a dummy `\n`. `session::write_and_drain` consumes that dummy so
the transport stays in sync without ever blocking.

### Remaining work

- **Milestone 4** (Second PR): TOML profile parser + `profiles/*.toml`. Note:
  the crate is currently dependency-free, so TOML parsing needs either a crate
  (`toml` + `serde`) or a small hand-rolled parser.
- **Milestone 5** (Second PR): generic `trigger_poll_fetch` workflow engine.
  Heads-up: the nRF52840 BLE scan commands differ from the ESP32-S3 demo
  (`BLE:SCAN:START`/`STOP`/`STATe?`/`COUNt?`/`RESult?`/`CLEar` vs the
  ESP32-S3 `BLE:SCAN`/`SCAN:DONE?`/`SCAN:COUNt?`/`SCAN?`), so workflow
  metadata and any `nrf52840.toml` profile must reflect the nRF command set.
- **Milestone 6** (Third PR): `SYST:HELP:DESC?` client + JSON parser +
  `describe` CLI, with clean fallback when the command is absent.
- **Milestone 7** (Fourth PR): firmware-side descriptor structs + the
  `SYSTem:HELP:DESCription?` core command returning a JSON block.
- **Milestone 8** (optional): `rusb` raw-USB backend behind a feature flag.

## Current State

The current ESP32-S3 host example is:

- `examples/esp32s3/host/scan_test.py`

It opens `/dev/usbtmcN` directly and relies on the Linux kernel USBTMC driver for framing. The script writes SCPI text lines and reads text responses.

Current limitations:

- Device-specific commands are hardcoded in Python:
  - `WLAN:SCAN`
  - `WLAN:SCAN:DONE?`
  - `WLAN:SCAN:COUNt?`
  - `WLAN:SCAN?`
  - `BLE:SCAN`
  - `BLE:SCAN:DONE?`
  - `BLE:SCAN:COUNt?`
  - `BLE:SCAN?`
  - `BLE:CONNect`
  - `BLE:PAIR`
  - `BLE:SEC?`
- The script assumes all responses are UTF-8-ish text.
- It cannot correctly handle binary block responses such as `DATA:READ?`.
- Adding a new device or workflow means editing Python code.

The device core already exposes useful generic commands in:

- `src/usbscpi.c`

Relevant core commands:

- `*IDN?`
- `*RST`
- `*CLS`
- `*OPC?`
- `SYSTem:ERRor?`
- `SYSTem:ERRor:COUNt?`
- `SYSTem:CAPabilities?`
- `SYSTem:HELP:HEADers?`
- `DATA:FREE?`
- `DATA:COUNt?`
- `DATA:READ?`

The ESP32-S3 demo command table is in:

- `examples/esp32s3/main/app_main.c`

That table proves the firmware already has a clear command registry. The missing piece is a machine-readable description of each command's parameters, response type, and workflow semantics.

## Design Decision

Do not make the first Rust version depend on a full descriptor being present.

Instead, implement three levels:

### Level 1: Generic SCPI Session

This is the first required milestone.

The host supports:

- open `/dev/usbtmcN`
- write SCPI commands
- query text responses
- query IEEE 488.2 definite-length arbitrary blocks
- write arbitrary blocks for `:DATA:WRITE`
- parse `*IDN?`
- parse `SYSTem:CAPabilities?`
- fetch command headers from `SYSTem:HELP:HEADers?`
- drain `SYSTem:ERRor?`

This level is device-independent.

### Level 2: Host Profile Fallback

Use a TOML profile to describe parameter and response metadata for known devices while firmware descriptor support is not available.

This allows the Rust host to replace `scan_test.py` quickly without blocking on firmware changes.

Example profile use cases:

- Wi-Fi scan workflow.
- BLE scan workflow.
- BLE connect and pair workflow.
- GPIO read/write commands.
- ADC read commands.

### Level 3: Device Descriptor

Add a device-side descriptor command as a compliant SCPI extension.

Recommended command:

- `SYSTem:HELP:DESCription?`

Short form accepted by the host:

- `SYST:HELP:DESC?`

The descriptor is not a complete built-in SCPI standard schema. It is an `iotsploit-usb` extension under the SCPI tree. It should return JSON or CBOR wrapped in an IEEE 488.2 definite-length arbitrary block.

This is still compatible with SCPI message framing because the payload is carried as arbitrary bytes inside a definite-length block.

## Repository Layout

Recommended initial location:

```text
examples/host-rs/
├── Cargo.toml
├── README.md
├── profiles/
│   └── esp32s3.toml
└── src/
    ├── lib.rs
    ├── transport.rs
    ├── usbtmc_kernel.rs
    ├── block.rs
    ├── session.rs
    ├── caps.rs
    ├── headers.rs
    ├── descriptor.rs
    ├── profile.rs
    ├── workflow.rs
    ├── repl.rs
    └── bin/
        └── iotsploit-host.rs
```

Reasoning:

- It keeps the host next to the examples it replaces.
- It does not disturb the existing C library or ESP-IDF component layout.
- It can be moved to a standalone crate later if it becomes a reusable tool.

## Rust Module Plan

### `transport.rs`

Define the transport abstraction.

```rust
pub trait Transport {
    fn write_msg(&mut self, bytes: &[u8]) -> Result<()>;
    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>>;
}
```

The transport should treat one write as one USBTMC device-dependent message and one read as one response message.

### `usbtmc_kernel.rs`

Linux MVP backend using `/dev/usbtmcN`.

Responsibilities:

- open path read/write
- auto-detect `/dev/usbtmc*`
- configure read timeout if practical
- expose clear errors when permission is denied

This backend matches the current Python behavior and avoids implementing raw USBTMC framing in the first phase.

### `block.rs`

Implement IEEE 488.2 definite-length arbitrary block support.

Required parser behavior:

- accept block header `#<digits><len>`
- read exactly `len` payload bytes
- preserve arbitrary bytes, including `0x00`, newline, carriage return, comma, quote, and `#`
- reject malformed headers
- reject lengths larger than configured maximum

Required encoder behavior:

- produce `#<digits><len><payload>`
- support `:DATA:WRITE #...` command construction

This is a major improvement over the current Python host because binary data must not be decoded as text.

### `session.rs`

High-level SCPI session.

```rust
pub struct ScpiSession<T: Transport> {
    transport: T,
    read_size: usize,
    max_block_len: usize,
}
```

Required methods:

- `write(cmd: &str) -> Result<()>`
- `query(cmd: &str) -> Result<String>`
- `query_raw(cmd: &str) -> Result<Vec<u8>>`
- `query_block(cmd: &str) -> Result<Vec<u8>>`
- `write_block(prefix: &str, payload: &[u8]) -> Result<()>`
- `drain_errors() -> Result<Vec<ScpiError>>`

Important behavior:

- Text commands should append exactly one newline.
- Text responses should trim trailing CR/LF only.
- Binary responses should never go through UTF-8 decoding.
- Errors should include the SCPI command that failed.

### `caps.rs`

Parse `SYSTem:CAPabilities?`.

Current firmware returns values shaped like:

```text
proto=1;mtu=256;maxblock=4096;feat=
```

The parser should be tolerant:

- unknown keys are retained
- missing keys use safe defaults
- numeric parse failures return structured errors

Result type:

```rust
pub struct Capabilities {
    pub proto: Option<u32>,
    pub mtu: Option<usize>,
    pub max_block: Option<usize>,
    pub features: Vec<String>,
    pub raw: String,
}
```

### `headers.rs`

Fetch command headers from `SYSTem:HELP:HEADers?`.

Responsibilities:

- support pagination with `offset count`
- handle current line-based response
- later support block-based response if firmware is tightened for stricter SCPI compatibility
- normalize command aliases and short/long forms for display

The host should attempt:

1. `SYST:HELP:HEAD? 0 64`
2. Continue paging until fewer than requested are returned.
3. Fall back to `SYST:HELP:HEAD?` with no pagination for older firmware.

### `descriptor.rs`

Parse the optional device descriptor returned by `SYST:HELP:DESC?`.

The host should treat descriptor support as optional:

- If present: use it for dynamic command validation, REPL help, and workflow generation.
- If absent: continue with headers plus profile.

Descriptor query strategy:

1. Query `SYST:HELP:DESC?`.
2. If response starts with `#`, parse as arbitrary block.
3. Decode JSON first.
4. If JSON is not supported later, allow CBOR behind a feature flag.
5. If the command errors with undefined header, mark descriptor as unavailable.

### `profile.rs`

TOML fallback for command metadata.

Profile data should be mergeable with device discovery:

- Discovery says what commands exist right now.
- Profile says what parameters and workflows mean.
- Descriptor, when present, has priority over profile.

The profile must not hide missing commands. If a profile references `BLE:PAIR` but the device does not advertise that header, the host should show it as unavailable.

### `workflow.rs`

Generic workflow engine for common command patterns.

First supported pattern:

```text
trigger -> poll done -> query count -> fetch indexed rows
```

This covers:

- `WLAN:SCAN`
- `BLE:SCAN`

Second supported pattern:

```text
trigger -> poll state -> handle interactive prompts -> query final result
```

This covers:

- `BLE:CONNect`
- `BLE:PAIR`

The workflow engine should be driven by descriptor/profile metadata, not Rust code hardcoded for Wi-Fi or BLE.

### `repl.rs`

Interactive mode.

Required features:

- list discovered commands
- show descriptor/profile help for one command
- run raw SCPI commands
- run named workflows
- print text responses
- save binary block responses to file

REPL should be useful for development but not required for automation.

### CLI Binary

Recommended binary name:

```text
iotsploit-host
```

MVP commands:

```text
iotsploit-host list
iotsploit-host idn
iotsploit-host caps
iotsploit-host headers
iotsploit-host query '*IDN?'
iotsploit-host write 'GPIO:SET 2,1'
iotsploit-host block-read 'DATA:READ? 64' --out adc.bin
iotsploit-host repl
```

Profile/workflow commands:

```text
iotsploit-host --profile profiles/esp32s3.toml workflow wifi-scan
iotsploit-host --profile profiles/esp32s3.toml workflow ble-scan --secs 5
iotsploit-host --profile profiles/esp32s3.toml workflow ble-connect-pair --index 0
```

Descriptor-first commands:

```text
iotsploit-host describe
iotsploit-host call GPIO:SET --pin 2 --value 1
iotsploit-host call ADC:READ --ch 0
```

## Descriptor Schema

Recommended wire format:

- command: `SYSTem:HELP:DESCription?`
- response: IEEE 488.2 definite-length arbitrary block
- payload: UTF-8 JSON
- versioned schema

Example response bytes:

```text
#41234{...1234 bytes JSON...}
```

Example JSON:

```json
{
  "schema": "iotsploit.scpi.descriptor.v1",
  "device": {
    "idn": "IoTSploit,ESP32S3,0001,0.1.0",
    "proto": 1,
    "mtu": 256,
    "max_block": 4096
  },
  "commands": [
    {
      "pattern": "GPIO:SET",
      "kind": "command",
      "summary": "Set GPIO output level",
      "params": [
        { "name": "pin", "type": "u32", "required": true },
        { "name": "value", "type": "bool", "required": true }
      ],
      "returns": { "type": "none" }
    },
    {
      "pattern": "GPIO:GET?",
      "kind": "query",
      "summary": "Read GPIO input level",
      "params": [
        { "name": "pin", "type": "u32", "required": true }
      ],
      "returns": { "type": "bool" }
    },
    {
      "pattern": "DATA:READ?",
      "kind": "query",
      "summary": "Read binary data from the device data source",
      "params": [
        { "name": "count", "type": "u32", "required": true, "max": 4096 }
      ],
      "returns": { "type": "block", "encoding": "raw" }
    }
  ],
  "workflows": [
    {
      "name": "wifi-scan",
      "type": "trigger_poll_fetch",
      "trigger": { "command": "WLAN:SCAN" },
      "done": { "query": "WLAN:SCAN:DONE?", "done_value": "1" },
      "count": { "query": "WLAN:SCAN:COUNt?" },
      "fetch": { "query": "WLAN:SCAN?", "index_param": "index" },
      "timeout_ms": 15000,
      "poll_ms": 250
    }
  ]
}
```

Required descriptor fields:

- `schema`
- `commands[].pattern`
- `commands[].kind`
- `commands[].params`
- `commands[].returns`

Optional descriptor fields:

- `summary`
- `params[].min`
- `params[].max`
- `params[].enum`
- `params[].default`
- `returns.fields`
- `workflows`
- `units`
- `examples`

Supported parameter types for v1:

- `bool`
- `u8`
- `u16`
- `u32`
- `i32`
- `float`
- `string`
- `bytes`
- `enum`

Supported return types for v1:

- `none`
- `bool`
- `u32`
- `i32`
- `float`
- `string`
- `csv`
- `block`

## Firmware Plan

Firmware changes should be staged after the Rust host MVP is working.

### Firmware Phase 1: Keep Existing Commands Stable

Do not change existing behavior for:

- `SYSTem:CAPabilities?`
- `SYSTem:HELP:HEADers?`
- `DATA:READ?`
- `DATA:FREE?`
- `DATA:COUNt?`
- ESP32-S3 WLAN/BLE commands

This lets the Rust host replace the Python host without firmware risk.

### Firmware Phase 2: Add Descriptor Types

Add a small metadata layer next to command registration.

Possible C shape:

```c
typedef enum {
    USBSCPI_PARAM_BOOL,
    USBSCPI_PARAM_U32,
    USBSCPI_PARAM_I32,
    USBSCPI_PARAM_STRING,
    USBSCPI_PARAM_BYTES
} usbscpi_param_type_t;

typedef enum {
    USBSCPI_RET_NONE,
    USBSCPI_RET_BOOL,
    USBSCPI_RET_U32,
    USBSCPI_RET_I32,
    USBSCPI_RET_STRING,
    USBSCPI_RET_CSV,
    USBSCPI_RET_BLOCK
} usbscpi_return_type_t;

typedef struct {
    const char *name;
    usbscpi_param_type_t type;
    bool required;
    int32_t min_value;
    int32_t max_value;
} usbscpi_param_desc_t;

typedef struct {
    const char *pattern;
    const char *summary;
    const usbscpi_param_desc_t *params;
    size_t param_count;
    usbscpi_return_type_t return_type;
} usbscpi_command_desc_t;
```

Avoid dynamic allocation. The descriptor should be generated from static tables into the existing `io_buf` or another caller-supplied buffer.

### Firmware Phase 3: Add `SYSTem:HELP:DESCription?`

Add a core command in `src/usbscpi.c`.

Implementation rules:

- No heap allocation.
- Use a bounded output buffer.
- Return JSON as a definite-length arbitrary block.
- If descriptor is too large for `mtu`, support pagination or chunking.
- If descriptor is larger than `max_block_len`, return a SCPI error.

Two viable approaches:

1. Single block if descriptor fits.
2. Paged descriptor:

```text
SYST:HELP:DESC? <offset>,<count>
```

For v1, prefer single block for simplicity. If the ESP32-S3 descriptor grows too large, add pagination before adding more metadata.

### Firmware Phase 4: Make HELP Headers More Strict Later

Current `SYSTem:HELP:HEADers?` returns line-separated text. That is workable and should not block the host.

For stricter SCPI alignment later, consider returning headers as a definite-length arbitrary block as well. The Rust host should support both the current text form and a future block form.

## Profile Fallback Plan

Create:

```text
examples/host-rs/profiles/esp32s3.toml
```

Example:

```toml
[device]
name = "esp32s3"

[[commands]]
pattern = "GPIO:SET"
kind = "command"
params = [
  { name = "pin", type = "u32", required = true },
  { name = "value", type = "bool", required = true },
]
returns = { type = "none" }

[[commands]]
pattern = "ADC:READ?"
kind = "query"
params = [
  { name = "ch", type = "u32", required = false, default = 0 },
]
returns = { type = "u32" }

[[workflows]]
name = "wifi-scan"
type = "trigger_poll_fetch"
trigger = "WLAN:SCAN"
done_query = "WLAN:SCAN:DONE?"
count_query = "WLAN:SCAN:COUNt?"
fetch_query = "WLAN:SCAN?"
timeout_ms = 15000
poll_ms = 250
```

The profile should be treated as host-side metadata, not as a substitute for device truth. The host must compare profile commands against discovered headers and warn when a profiled command is missing.

## Development Milestones

### Milestone 0: Planning and Skeleton — Status: DONE

Deliverables:

- Add `examples/host-rs/` Cargo project.
- Add module skeletons.
- Add README with usage.
- Add CI/build instructions.

Acceptance:

- `cargo check` passes.
- `iotsploit-host --help` works.

Estimated effort: 0.5 to 1 day.

### Milestone 1: Linux `/dev/usbtmc` Transport — Status: DONE

Deliverables:

- `Transport` trait.
- `UsbtmcKernel` backend.
- device auto-detection.
- clear permission and missing-device errors.

Acceptance:

- `iotsploit-host --device /dev/usbtmc0 query '*IDN?'` works on hardware.
- auto-detect works when exactly one `/dev/usbtmc*` exists.

Estimated effort: 1 day.

### Milestone 2: SCPI Session and Text Queries — Status: DONE

Deliverables:

- `ScpiSession`.
- `write`, `query`, `query_raw`.
- error queue drain.
- `idn`, `caps`, and `headers` CLI commands.

Acceptance:

- `*IDN?` returns expected identity.
- `SYST:CAP?` parses into structured fields.
- `SYST:HELP:HEAD?` lists core and app commands.
- `SYST:ERR?` can be drained.

Estimated effort: 1 to 1.5 days.

### Milestone 3: Binary Block Support — Status: DONE

Deliverables:

- `block.rs` parser and encoder.
- `query_block`.
- `write_block`.
- CLI support for saving block output to a file.

Acceptance:

- `iotsploit-host block-read 'DATA:READ? 64' --out adc.bin` writes exactly the payload bytes.
- malformed block unit tests pass.
- oversized block is rejected before allocation pressure.

Estimated effort: 1 to 1.5 days.

### Milestone 4: Profile Metadata — Status: TODO

Deliverables:

- TOML profile parser.
- `profiles/esp32s3.toml`.
- command metadata merge with discovered headers.
- validation warnings for missing commands.

Acceptance:

- Host can show typed help for GPIO, ADC, WLAN, and BLE commands.
- Host can reject missing required params before sending invalid SCPI.
- Profile commands are marked unavailable if missing from `SYST:HELP:HEAD?`.

Estimated effort: 1.5 to 2 days.

### Milestone 5: Workflow Engine — Status: TODO

Deliverables:

- generic `trigger_poll_fetch` workflow.
- Wi-Fi scan workflow.
- BLE scan workflow.
- BLE connect/pair interactive workflow.

Acceptance:

- Rust host reproduces `scan_test.py --wifi-only`.
- Rust host reproduces `scan_test.py --ble-only`.
- Rust host supports connect by index.
- Rust host supports passkey and numeric comparison prompts.

Estimated effort: 2 to 3 days.

### Milestone 6: Descriptor Client Support — Status: TODO

Deliverables:

- `SYST:HELP:DESC?` query support.
- JSON descriptor parser.
- descriptor/profile/discovery merge logic.
- CLI `describe` command.

Acceptance:

- If descriptor command is missing, host falls back cleanly.
- If descriptor is present, descriptor metadata overrides profile metadata.
- Invalid descriptor gives a useful error and does not break raw SCPI mode.

Estimated effort: 1.5 to 2 days.

### Milestone 7: Firmware Descriptor Support — Status: TODO

Deliverables:

- static descriptor metadata structs.
- core `SYSTem:HELP:DESCription?` command.
- JSON block response.
- descriptor tests.

Acceptance:

- `SYST:HELP:DESC?` returns a valid definite-length block.
- JSON validates against schema v1.
- Host can build typed command help from device descriptor without profile.
- Existing commands remain compatible.

Estimated effort: 2 to 4 days depending on descriptor size and pagination needs.

### Milestone 8: Optional Raw USB Backend — Status: TODO (optional)

Deliverables:

- `rusb` backend behind a cargo feature.
- USBTMC bulk OUT/IN framing.
- device selection by VID/PID/serial.

Acceptance:

- Linux backend still works through `/dev/usbtmcN`.
- Raw backend can communicate without the kernel USBTMC character device.

Estimated effort: 3 to 5 days.

This is not required for the first useful Rust replacement.

## Testing Plan

### Rust Unit Tests

Required unit tests:

- block parser accepts valid `#<digits><len>` blocks.
- block parser rejects malformed headers.
- block parser rejects too-large lengths.
- capabilities parser handles normal and unknown keys.
- descriptor parser validates required fields.
- profile parser loads `esp32s3.toml`.
- workflow formatter builds indexed query commands correctly.

### Rust Integration Tests Without Hardware

Use a fake `Transport` implementation.

Test flows:

- `query("*IDN?")`
- `caps()`
- `headers()` with paged responses
- `query_block("DATA:READ? 64")`
- workflow `trigger_poll_fetch`
- missing descriptor fallback

### Hardware Smoke Tests

Run against ESP32-S3 over `/dev/usbtmcN`.

Required tests:

```text
iotsploit-host idn
iotsploit-host caps
iotsploit-host headers
iotsploit-host query 'ADC:READ?'
iotsploit-host write 'GPIO:SET 2,1'
iotsploit-host query 'GPIO:GET? 2'
iotsploit-host block-read 'DATA:READ? 64' --out adc.bin
iotsploit-host workflow wifi-scan --profile profiles/esp32s3.toml
iotsploit-host workflow ble-scan --profile profiles/esp32s3.toml --secs 5
```

Compare Wi-Fi and BLE output against the existing Python script before removing or deprecating it.

### Firmware Tests

Existing C tests should continue to pass.

Add tests for:

- `SYST:HELP:DESC?` returns block format.
- descriptor JSON includes all registered command patterns.
- descriptor command fails cleanly when output buffer is too small.
- existing `SYST:HELP:HEAD?` behavior remains unchanged for compatibility.

## Compatibility Rules

1. Do not break existing SCPI command strings.
2. Do not require descriptor support for raw SCPI query/write mode.
3. Do not decode binary blocks as strings.
4. Keep `/dev/usbtmcN` as the first backend.
5. Treat descriptor as optional enhancement.
6. Keep profile fallback until all target firmware exposes descriptor.
7. Keep `scan_test.py` until Rust host has hardware parity.

## Risks and Mitigations

### Risk: Descriptor Becomes Too Large for `mtu`

Mitigation:

- Return descriptor as a block.
- Enforce `max_block_len`.
- Add pagination if needed.

### Risk: Host Assumes Too Much from Command Names

Mitigation:

- Never infer parameter types from names alone.
- Use descriptor or profile for parameters.
- Use headers only to know what exists.

### Risk: Firmware Metadata Duplicates Command Table

Mitigation:

- Keep descriptor fields small in v1.
- Consider macros later to define command callback and descriptor together.
- Start with static hand-written descriptors for ESP32-S3 only.

### Risk: Breaking Current Python Workflow

Mitigation:

- Make firmware descriptor additive.
- Do not change existing command output in the first firmware phase.
- Use hardware comparison with `scan_test.py`.

### Risk: Linux Permissions

Mitigation:

- Print clear error for permission denied.
- Document udev rules later.
- Use `sudo` only as a temporary workaround.

## Proposed Implementation Order

Recommended order:

1. Rust host skeleton under `examples/host-rs/`.
2. `/dev/usbtmc` transport.
3. SCPI text session.
4. capabilities and headers discovery.
5. binary block support.
6. `esp32s3.toml` profile.
7. Wi-Fi and BLE workflows.
8. descriptor client support in Rust.
9. firmware `SYST:HELP:DESC?`.
10. profile becomes fallback instead of primary metadata.
11. optional raw USB backend.

This order gives useful value before firmware changes and keeps risk low.

## First PR Scope — Status: DONE

The first PR should not touch firmware descriptor support.

Recommended first PR contents:

- `examples/host-rs/Cargo.toml`
- `examples/host-rs/src/transport.rs`
- `examples/host-rs/src/usbtmc_kernel.rs`
- `examples/host-rs/src/session.rs`
- `examples/host-rs/src/block.rs`
- `examples/host-rs/src/caps.rs`
- `examples/host-rs/src/headers.rs`
- `examples/host-rs/src/bin/iotsploit-host.rs`
- unit tests for block and caps parsing
- README with basic commands

First PR acceptance:

- `cargo test` passes.
- On hardware, these commands work:
  - `iotsploit-host idn`
  - `iotsploit-host caps`
  - `iotsploit-host headers`
  - `iotsploit-host query 'ADC:READ?'`
  - `iotsploit-host block-read 'DATA:READ? 64' --out adc.bin`

## Second PR Scope — Status: TODO

Recommended second PR contents:

- profile parser
- `profiles/esp32s3.toml`
- workflow engine
- Wi-Fi scan workflow
- BLE scan workflow
- BLE connect/pair workflow

Second PR acceptance:

- Rust host can replace `scan_test.py` for the current ESP32-S3 demo.
- Existing Python script remains available for comparison.

## Third PR Scope — Status: TODO

Recommended third PR contents:

- descriptor schema document
- descriptor client support in Rust
- optional `describe` CLI command
- fake transport tests for descriptor present and descriptor missing

Third PR acceptance:

- Host can consume descriptor if firmware provides one.
- Host still works with current firmware where descriptor is missing.

## Fourth PR Scope — Status: TODO

Recommended fourth PR contents:

- firmware descriptor metadata structs
- `SYSTem:HELP:DESCription?`
- JSON block output
- C tests for descriptor command
- ESP32-S3 descriptor entries for GPIO, ADC, WLAN, BLE, and DATA commands

Fourth PR acceptance:

- Rust host can control ESP32-S3 with descriptor only, without `esp32s3.toml`.
- Profile remains as fallback.

## Definition of Done

The project is done when:

- One Rust host binary can talk to the ESP32-S3 demo over `/dev/usbtmcN`.
- The binary supports raw SCPI write/query for any discovered command.
- The binary supports binary block read/write without text decoding.
- The binary can list device capabilities and command headers.
- Wi-Fi scan and BLE scan work from generic workflow metadata.
- BLE connect/pair works with interactive prompts.
- Descriptor support is optional but implemented end to end.
- New devices can be supported by firmware descriptor alone or by adding a TOML profile.
- Existing SCPI commands and Python smoke test behavior are not broken.

