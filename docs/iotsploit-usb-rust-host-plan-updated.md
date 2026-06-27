# iotsploit-usb Generic Rust Host Plan (Updated 2026-06-26)

## Summary

Build a generic Rust host for `iotsploit-usb` that can control devices through
SCPI over USBTMC without hardcoding every board-specific workflow in Rust code.

**Current state**: Milestones 0–4 are **complete and merged to `main`**. The
Rust host currently lives at `examples/host-rs/` and is a fully functional,
dependency-free Linux CLI tool through the kernel `/dev/usbtmcN` interface. The
`feat/host-rs` branch pointer currently matches `main` — all host code was
merged directly.

**Directory restructure (Decided 2026-06-26, Layout A)**: The Rust host is a
first-class deliverable, not a demo, so it moves out of `examples/` into a
dedicated top-level `host/` directory that groups *all* upper-computer
implementations by language. The existing Python host (`scan_test.py`) moves
alongside it. See [Repository Layout](#repository-layout-decided-2026-06-26)
below. This restructure ships as its own `refactor/host-layout` PR *before* the
Milestone 5 work begins.

**Updated requirement**: the Rust host must support **Linux, Windows, and
macOS**. Linux should keep the existing `/dev/usbtmcN` backend because it is
simple and already working. Windows and macOS need a raw USBTMC backend based on
USB bulk transfers, implemented behind the same `Transport` trait.

---

## Repository Layout (Decided 2026-06-26)

The Rust host moves from `examples/host-rs/` to a dedicated top-level `host/`
directory. `host/` is the home for *all* upper-computer implementations, split
by language, so `examples/` is reserved for firmware demos only.

**Target layout (Layout A):**

```
iotsploit-usb/
├── host/
│   ├── rust/               # was examples/host-rs/ — moved wholesale
│   │   ├── Cargo.toml
│   │   ├── README.md
│   │   ├── src/
│   │   └── profiles/       # Milestone 5: esp32s3.toml / nrf52840.toml
│   ├── python/             # scan_test.py and any other Python host code
│   └── README.md           # explains the role of each host implementation
└── examples/               # firmware-only demos (esp32s3, nrf52840, ...)
```

**Rationale**: by its planned scope (multiple transport backends, profiles,
workflows, descriptor client, cross-platform release artifacts) the Rust host is
a first-class product, not an example. `host/rust` + `host/python` keeps each
host independent and gives the directory clean, future-proof semantics.

**Migration rules** (single `refactor/host-layout` PR, no functional changes):

1. `git mv examples/host-rs host/rust` and `git mv scan_test.py host/python/`
   (adjust for the real Python host path) to preserve commit history.
2. The crate is path-relative and zero-dep, so `cargo build`/`cargo test` need
   no changes after the move; but update any workspace `Cargo.toml` `members`,
   `README.md` example paths, and CI/scripts that hard-code `examples/host-rs`.
3. Add `host/README.md` describing the Rust vs Python host roles.
4. Keep this restructure isolated from the Milestone 5/6 feature work — it lands
   first, on its own branch, off `main`.
5. Firmware example paths (`examples/esp32s3/...`, `examples/nrf52840/...`) stay
   put; only host code moves.

The architecture trees and paths elsewhere in this plan reflect the post-move
`host/rust/` location.

---

## Implementation Status

| Milestone | Scope | Status | Notes |
|---|---|---|---|
| 0 | Planning & skeleton | ✅ DONE | `cargo check` passes, `--help` works |
| 1 | Linux `/dev/usbtmc` transport | ✅ DONE | `UsbtmcKernel` with auto-detect |
| 2 | SCPI session & text queries | ✅ DONE | `write`, `query`, `drain_errors` |
| 3 | Binary block support | ✅ DONE | `block.rs` parser/encoder, `query_block`, `write_block` |
| 4 | Command discovery & REPL | ✅ DONE | `headers.rs`, interactive REPL |
| R | Directory restructure → `host/rust` | ✅ DONE | Layout A; `host/rust` + `host/python` |
| 5 | Profile metadata (line records) | ✅ DONE | Zero-dep `TAG key=value` parser, shared with M7 |
| 6 | Workflow engine | ✅ DONE | `trigger_poll_fetch` + `trigger_poll_interactive` |
| 7 | Descriptor client (Rust) | ✅ DONE | `SYST:HELP:DESC?` line-record consumer, shared parser |
| 8 | Firmware descriptor (`SYST:HELP:DESC?`) | ✅ DONE | `snprintf` line records, no heap, IEEE 488.2 block |
| 9 | Desktop cross-platform USB backend | ✅ DONE | `UsbtmcRaw` via `rusb`/libusb, `--backend` selector |
| 10 | Packaging and platform docs | ✅ DONE | Linux udev, Windows driver, macOS permissions |

---

## What's Built (Milestones 0–4)

### Architecture

```
host/rust/                  # (was examples/host-rs/ — see Repository Layout)
├── Cargo.toml              # zero external dependencies
├── README.md               # comprehensive usage guide
├── src/
│   ├── lib.rs              # Error enum, Result alias
│   ├── transport.rs        # Transport trait (write_msg / read_msg)
│   ├── usbtmc_kernel.rs    # /dev/usbtmcN backend (open, auto_detect)
│   ├── block.rs            # IEEE 488.2 definite-length block parse/encode
│   ├── session.rs          # ScpiSession: write, query, query_block, write_block, drain_errors
│   ├── caps.rs             # SYSTem:CAPabilities? parser (tolerant, handles %zu quirk)
│   ├── headers.rs          # SYSTem:HELP:HEADers? fetch with paged fallback
│   └── bin/
│       └── iotsploit-host.rs  # CLI binary
└── target/
    ├── debug/              # debug build artifacts
    └── release/
        └── iotsploit-host  # optimized binary (~0 external deps = fast build)
```

### Key Design Decisions

1. **Zero dependencies** — the entire crate builds with only `std`. No `serde`,
   `toml`, `clap`, or `anyhow`. CLI parsing is hand-rolled. This means the host
   builds anywhere a Rust toolchain exists, including offline/embedded dev
   machines.

2. **Transport trait** — `write_msg`/`read_msg` abstraction allows future
   backends (`rusb`, TCP/VISA) without changing session logic.

3. **Binary correctness** — `block.rs` parses IEEE 488.2 blocks at the byte
   level, never decoding binary as UTF-8. This is a hard improvement over the
   Python host which does `os.read(...).decode()`.

4. **Tolerant parsing** — `caps.rs` handles the nRF52840 libc quirk where
   `%zu` prints literally as `zu`. Instead of failing, it records parse errors
   and falls back to safe defaults (mtu=256, maxblock=4096).

5. **`write_and_drain`** — non-query SCPI commands produce no real output, but
   the TinyUSB glue answers every kernel IN request with a dummy `\n`. The
   session consumes that dummy to keep the transport in sync.

### CLI Commands

```
iotsploit-host list                          # list /dev/usbtmc* nodes
iotsploit-host idn                           # *IDN?
iotsploit-host caps                          # SYSTem:CAPabilities? (parsed)
iotsploit-host headers                       # SYSTem:HELP:HEADers? (all)
iotsploit-host query '<cmd>'                 # text query
iotsploit-host write '<cmd>'                 # non-query command
iotsploit-host block-read '<cmd>' [--out F]  # binary block → file/stdout
iotsploit-host errors                        # drain SYSTem:ERRor? queue
iotsploit-host repl                          # interactive SCPI prompt
```

---

## Updated Cross-Platform Requirement: Linux, Windows, macOS

The host should have one shared Rust SCPI/session/workflow stack and multiple
transport backends.

### Required Transport Strategy

```
                 ┌──────────────────────────────┐
                 │ shared Rust host core         │
                 │ session/block/caps/headers    │
                 │ profile/workflow/descriptor   │
                 └───────────────┬──────────────┘
                                 │ Transport trait
         ┌───────────────────────┼───────────────────────┐
         │                       │                       │
 ┌───────▼────────┐      ┌───────▼────────┐      ┌───────▼────────┐
 │ Linux kernel   │      │ Raw USBTMC     │      │ future network │
 │ /dev/usbtmcN   │      │ rusb/libusb    │      │ TCP/VISA bridge│
 │ already built  │      │ Win/Linux/macOS│      │ optional       │
 └────────────────┘      └────────────────┘      └────────────────┘
```

The existing `UsbtmcKernel` backend stays as the fastest Linux path. The new
cross-platform backend should be `UsbtmcRaw`, built on `rusb`/libusb and the
USBTMC bulk protocol.

### Backend Selection

Recommended CLI behavior:

```text
iotsploit-host --backend auto idn
iotsploit-host --backend kernel --device /dev/usbtmc0 idn
iotsploit-host --backend raw --vid 1209 --pid 0001 idn
iotsploit-host --backend raw --serial 0001 headers
```

`auto` should mean:

1. On Linux, try `/dev/usbtmcN` first if present.
2. If no kernel node exists, try raw USB backend.
3. On Windows and macOS, use raw USB backend.

### Raw USBTMC Backend Scope

`UsbtmcRaw` must implement USBTMC device-dependent message transfers directly:

- enumerate USB devices by VID/PID/interface class
- find USBTMC interface and bulk IN/OUT endpoints
- claim the interface
- send DEV_DEP_MSG_OUT with USBTMC header
- read DEV_DEP_MSG_IN response packets
- reassemble multi-packet responses
- honor EOM
- implement clear/abort later if needed
- expose timeouts consistently across platforms

This backend should still implement the existing `Transport` trait:

```rust
pub trait Transport {
    fn write_msg(&mut self, bytes: &[u8]) -> Result<()>;
    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>>;
}
```

The `ScpiSession`, block parser, capabilities parser, headers fetcher, profile
engine, workflow engine, and descriptor client must not care which backend is
used.

### Linux Support

Linux has two supported paths:

1. **Kernel backend**: `/dev/usbtmcN`, already implemented and verified.
2. **Raw backend**: libusb/rusb, useful when the kernel driver is unavailable
   or when behavior must match Windows/macOS.

Linux requirements:

- keep existing `/dev/usbtmcN` behavior stable
- add udev documentation for non-root access
- raw backend should handle kernel-driver-detach only when needed and only when
  explicitly selected or documented
- `auto` mode should prefer kernel backend because it is already stable

Linux acceptance:

- `iotsploit-host --backend kernel idn` works
- `iotsploit-host --backend raw --vid 1209 --pid 0001 idn` works
- `query`, `write`, `headers`, `block-read`, and workflows behave the same on
  both backends

### Windows Support

Windows does not provide a `/dev/usbtmcN` equivalent. The Rust host needs the
raw USB backend.

Windows requirements:

- support `x86_64-pc-windows-msvc` first
- use `rusb`/libusb for USB access
- document driver binding requirements:
  - WinUSB or libusbK through Zadig, or
  - a project-provided INF later
- device selection by VID/PID and serial
- no dependency on NI-VISA for the first Windows implementation

Windows acceptance:

- `iotsploit-host.exe --backend raw --vid 1209 --pid 0001 idn` works
- `headers`, text `query`, non-query `write`, and `block-read` work
- permission/driver errors explain the WinUSB/libusbK requirement

### macOS Support

macOS also needs the raw USB backend.

macOS requirements:

- support Apple Silicon and Intel:
  - `aarch64-apple-darwin`
  - `x86_64-apple-darwin`
- use `rusb`/libusb through Homebrew or bundled packaging later
- document device permission and code-signing/notarization constraints
- avoid relying on Linux-specific device paths or ioctls

macOS acceptance:

- `iotsploit-host --backend raw --vid 1209 --pid 0001 idn` works on Apple
  Silicon
- same command set passes on Intel when hardware is available
- `headers`, `query`, `write`, and `block-read` match Linux behavior

### Cross-Platform Build Matrix

Required CI/build targets:

```text
x86_64-unknown-linux-gnu
x86_64-pc-windows-msvc
x86_64-apple-darwin
aarch64-apple-darwin
```

Optional later:

```text
aarch64-linux-android
x86_64-linux-android
```

Recommended cargo features:

```text
default = ["kernel"]
kernel = []              # Linux /dev/usbtmcN backend
raw-usb = ["rusb"]       # Windows/Linux/macOS raw USBTMC backend
profiles = ["serde", "toml"]
descriptor-json = ["serde", "serde_json"]
```

For Windows/macOS release builds, `raw-usb` should be enabled.

### Dependency Decision Update

The current host has zero external dependencies. Cross-platform USB changes
that tradeoff.

Recommendation:

- keep the core session/block/caps/headers code dependency-light
- add `rusb` behind a `raw-usb` feature
- add `serde`/`toml` behind a `profiles` feature
- add `serde_json` behind a `descriptor-json` feature

This keeps the simple Linux kernel backend small while allowing real
Windows/macOS support.

### Hardware Verification (nRF52840)

All commands verified on nRF52840 (`VID 1209:0001`, `/dev/usbtmc0`):

- `idn` → `IoTSploit,nRF52840,0001,0.1.0`
- `caps` → tolerates `mtu=zu` quirk, reports `proto=Some(1)`
- `headers` → all 17 registered commands
- `query 'BLE:SCAN:STATe?'` → `0`
- `write 'BLE:SCAN:CLEar'` → returns in ~21ms, no hang, no read desync
- `block-read 'DATA:READ? 64' --out /tmp/adc.bin` → 0-byte file (empty block,
  expected — no data source wired on this board)
- `errors` → `(no errors)`

### Tests

32+ unit and fake-transport integration tests pass (`cargo test`):

- Block parser: valid blocks, empty blocks, malformed headers, too-large,
  payload-too-short, roundtrip encode/parse
- Caps parser: normal response, features list, `%zu` tolerance, unknown keys,
  missing keys, empty input, whitespace
- Headers fetch: unpaged short-circuit, paged fallback, short-page termination
- Session: query trims newlines, block returns payload only, write appends
  newline, drain_errors stops at "No error", write_block constructs correct
  message

---

## Remaining Work

### Milestone 5: Profile Metadata (TOML) — Second PR

**Goal**: Describe command parameters and workflows in a TOML file so the host
can provide typed help and parameter validation without firmware changes.

**Deliverables**:
- TOML profile parser (needs `toml`+`serde` crates, or a small hand-rolled
  parser to maintain zero-dep status)
- `profiles/esp32s3.toml` — Wi-Fi scan, BLE scan, BLE connect/pair, GPIO, ADC
- `profiles/nrf52840.toml` — BLE scan (different command set than ESP32-S3!)
- Command metadata merge with discovered headers
- Validation: warn when profile references a command not in `SYST:HELP:HEAD?`

**Key challenge**: The nRF52840 and ESP32-S3 BLE scan commands differ:
- nRF52840: `BLE:SCAN:START`/`STOP`/`STATe?`/`COUNt?`/`RESult?`/`CLEar`
- ESP32-S3: `BLE:SCAN`/`SCAN:DONE?`/`SCAN:COUNt?`/`SCAN?`

The profile format must handle both, and the workflow engine must be generic
enough to drive either pattern.

**Estimated effort**: 1.5–2 days

### Milestone 6: Workflow Engine — Second PR (same)

**Goal**: Generic `trigger → poll → count → fetch` engine driven by
profile/descriptor metadata.

**Pattern 1: trigger_poll_fetch** (Wi-Fi scan, BLE scan)
```
write trigger_cmd
loop: query done_query until done_value
query count_query
for i in 0..count: query fetch_query(i)
```

**Pattern 2: trigger_poll_interactive** (BLE connect/pair)
```
write trigger_cmd
loop: query state, handle passkey/numcmp prompts
query final result
```

**Deliverables**:
- `workflow.rs` with `run_trigger_poll_fetch()`
- REPL integration: `workflow wifi-scan`, `workflow ble-scan`
- Shell-friendly output (one result per line, machine-parseable)

**Estimated effort**: 2–3 days

### Milestone 7: Descriptor Client — Third PR

**Goal**: Rust host can consume an optional `SYST:HELP:DESC?` response and use
it instead of profile metadata.

**Deliverables**:
- `descriptor.rs` — query `SYST:HELP:DESC?`, parse IEEE 488.2 block, decode
  JSON
- Descriptor schema v1 validation
- Merge logic: descriptor > profile > headers-only
- CLI `describe` command
- Clean fallback when command is absent (SCPI error → mark unavailable)

**Descriptor JSON schema** (proposed):
```json
{
  "schema": "iotsploit.scpi.descriptor.v1",
  "device": { "idn": "...", "proto": 1, "mtu": 256, "max_block": 4096 },
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

**Estimated effort**: 1.5–2 days

### Milestone 8: Firmware Descriptor — Fourth PR

**Goal**: Device-side `SYSTem:HELP:DESCription?` returning JSON in an IEEE
488.2 definite-length arbitrary block.

**Deliverables**:
- Static descriptor metadata structs in C (`usbscpi_param_desc_t`,
  `usbscpi_command_desc_t`)
- Core `SYSTem:HELP:DESCription?` command in `src/usbscpi.c`
- JSON serialization into caller-supplied buffer (no heap)
- ESP32-S3 descriptor entries for GPIO, ADC, WLAN, BLE, DATA commands
- C tests

**Implementation rules**:
- No heap allocation
- Bounded output buffer
- Return as definite-length arbitrary block (SCPI-compliant)
- If descriptor exceeds `max_block_len`, return SCPI error (add pagination
  later if needed)
- Existing `SYST:HELP:HEAD?` behavior unchanged

**Estimated effort**: 2–4 days

### Milestone 9: Desktop Cross-Platform USB — Required

**Goal**: `rusb` (libusb) backend for Windows, Linux, and macOS.

**Deliverables**:
- `UsbtmcRaw` transport implementing USBTMC bulk-OUT/IN framing
- 12-byte USBTMC header construction
- USB enumeration by VID/PID/serial
- interface claiming and endpoint discovery
- response reassembly across multiple USBTMC packets
- EOM handling
- timeouts and clear errors across platforms
- feature-gated build: `cargo build --features raw-usb`
- backend selector: `--backend auto|kernel|raw`
- Linux parity test: kernel backend vs raw backend
- Windows test through WinUSB/libusbK
- macOS test through libusb

**Platform rules**:
- Linux keeps `/dev/usbtmcN` as the default path.
- Windows uses raw USB only.
- macOS uses raw USB only.
- Android remains optional and should not block desktop support.

**Estimated effort**: 4–7 days

### Milestone 10: Packaging and Platform Docs — Required

**Goal**: Make the cross-platform host practical to install and run.

**Deliverables**:
- Linux udev rule example for non-root `/dev/usbtmcN`
- Windows driver setup doc for WinUSB/libusbK via Zadig or project INF
- macOS libusb install instructions and notarization/signing notes
- release artifact naming:
  - `iotsploit-host-linux-x86_64`
  - `iotsploit-host-windows-x86_64.exe`
  - `iotsploit-host-macos-x86_64`
  - `iotsploit-host-macos-aarch64`
- smoke-test checklist per OS

**Estimated effort**: 1–2 days

---

## Updated Architecture Diagram

```
                         ┌──────────────────────┐
                         │   iotsploit-host CLI  │
                         │   (iotsploit-host.rs) │
                         └──────────┬───────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    │               │               │
              ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐
              │  session   │  │   caps    │  │  headers  │
              │ write/query│  │  parse    │  │  fetch    │
              └─────┬──────┘  └───────────┘  └───────────┘
                    │
              ┌─────▼──────┐
              │   block    │  IEEE 488.2 parse/encode
              └─────┬──────┘
                    │
              ┌─────▼──────┐
              │ transport  │  trait: write_msg / read_msg
              └─────┬──────┘
                    │
         ┌──────────┼──────────┐
         │          │          │
   ┌─────▼─────┐ ┌─────▼─────┐ ┌────▼────┐
   │usbtmc_    │ │usbtmc_raw │ │ (future │
   │kernel     │ │rusb/libusb│ │  tcp)   │
   │Linux only │ │Win/Lin/mac│ │         │
   └───────────┘ └───────────┘ └─────────┘
         │
    USB cable (USBTMC)
         │
   ┌─────▼──────────┐
   │ iotsploit-usb  │
   │ firmware       │
   │ (libscpi +     │
   │  TinyUSB)      │
   └────────────────┘
```

Future additions (profile, workflow, descriptor) sit between the CLI and
session layers:

```
   CLI → profile → workflow → session → transport → device
              ↑
         descriptor (optional, overrides profile)
```

---

## Compatibility Rules (Unchanged)

1. Do not break existing SCPI command strings.
2. Do not require descriptor support for raw SCPI query/write mode.
3. Do not decode binary blocks as strings.
4. Keep `/dev/usbtmcN` stable as the Linux default backend.
5. Treat descriptor as optional enhancement.
6. Keep profile fallback until all target firmware exposes descriptor.
7. Keep `scan_test.py` (now under `host/python/`) until Rust host has hardware
   parity for ESP32-S3.
8. Use the same host core on Linux, Windows, and macOS; only the transport
   backend should vary by platform.
9. Windows/macOS support must not require NI-VISA in the first implementation.

---

## Risk Update

| Risk | Status | Notes |
|---|---|---|
| Descriptor too large for mtu | Mitigated | Block response bypasses mtu; use `max_block_len` |
| Host assumes too much from names | Mitigated | Never infer types from names; use descriptor/profile |
| Breaking Python workflow | Mitigated | Rust host is additive; Python still works |
| Linux permissions | Mitigated | Clear error messages; udev rule documented |
| nRF52840 vs ESP32-S3 command差异 | Active | Profile format must handle both command sets |
| Zero-dep vs TOML parsing | Decision needed | Hand-roll tiny TOML parser or accept `toml`+`serde` dep |
| Windows USB driver binding | Active | Document WinUSB/libusbK setup; no NI-VISA dependency in v1 |
| macOS USB permissions/packaging | Active | Document libusb install; handle signing/notarization later |
| Kernel/raw backend behavior drift | Active | Add shared fake-transport tests plus Linux hardware parity tests |

---

## Next Steps (Recommended)

1. **Directory restructure first** (`refactor/host-layout` PR): move
   `examples/host-rs/` → `host/rust/` and the Python host → `host/python/` per
   [Repository Layout](#repository-layout-decided-2026-06-26). No functional
   changes; land it before any Milestone 5 work so later PRs branch from the new
   paths.

2. **Decide on branch strategy**: The `feat/host-rs` branch currently matches
   `main`. Future work (profiles, workflows, descriptor) should use feature
   branches off `main`.

3. **Second PR** (Milestones 5+6): Profile + Workflow — this is the highest
   value next step because it lets the Rust host replace `scan_test.py` for
   both ESP32-S3 and nRF52840.

4. **TOML dependency decision**: Either add `toml`+`serde` (ergonomic, ~2 new
   deps) or hand-roll a minimal TOML parser (keeps zero-dep status, more code
   to maintain). Recommendation: add the crates — TOML parsing is not trivial
   to get right, and the build cost is negligible.

5. **Profile authoring**: Create `host/rust/profiles/esp32s3.toml` and
   `host/rust/profiles/nrf52840.toml` based on the existing command tables in
   `examples/esp32s3/main/app_main.c` and `examples/nrf52840/ble_scan_handler.c`.

6. **Third PR** (Milestones 7+8): Descriptor end-to-end — Rust client + firmware
   `SYST:HELP:DESC?`.

7. **Fourth PR** (Milestones 9+10): Desktop cross-platform backend —
   `UsbtmcRaw` using `rusb`/libusb, with Linux raw parity, Windows WinUSB/libusbK
   docs, macOS libusb docs, and release artifact notes.
