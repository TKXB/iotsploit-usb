# Adding a USB Log-Output Endpoint (Option B: Vendor Interface + Bulk-IN)

> **Status: implemented and hardware-verified.** ESP32-S3 firmware, host
> `UsbtmcLogReader`, the `usbtmc_log_stream` bridge, and the Flutter "Device Log"
> tab are all in place. On real ESP32-S3 hardware the device enumerates two
> interfaces (USBTMC on iface 0, vendor log on iface 1 / EP `0x82`), `*IDN?`
> still answers on `/dev/usbtmc3`, and the log stream delivers live `ESP_LOGx`
> output concurrently with SCPI, including across reconnects. Two non-obvious
> bugs were found and fixed during bring-up — see **Two bugs that block the log
> stream** below; if the Device Log tab is empty, check those first.

## Goal

Add an independent, always-on, push-style device→host **log stream** without
disturbing the existing USBTMC command/response path.

The log stream lives on a **new, second USB interface** (vendor class `0xFF`)
with its own bulk-IN endpoint. Interface 0 (USBTMC) is left byte-for-byte
unchanged, so the Linux kernel `usbtmc` driver and the existing host code keep
working exactly as today.

---

## Why a second interface (and not an endpoint on the USBTMC interface)

The device advertises interface 0 as class `0xFE / 0x03 / USB488` with exactly
the standard USBTMC endpoints — bulk-OUT `0x01`, bulk-IN `0x81`
(`examples/nrf52840/usb_descriptors.c`). That interface is claimed as a unit by:

- the **Linux kernel `usbtmc` driver**, which the primary host backend depends
  on — `UsbtmcKernel` just opens `/dev/usbtmcN` (`host/rust/src/usbtmc_kernel.rs`);
- **TinyUSB's `usbtmc` class driver**, which only manages the standard endpoint set.

An extra non-standard bulk endpoint added *to the USBTMC interface itself* is
non-conformant: the kernel driver won't surface it, TinyUSB won't service it, and
it risks breaking the command path. USB488's only optional extra endpoint is an
interrupt-IN for SRQ notifications (small packets), which is the wrong tool for a
bulk log stream.

The clean approach is a **composite device**: keep interface 0 as pure USBTMC,
add interface 1 (vendor) that owns the log endpoint.

---

## Endpoint map: before and after

### Before (USBTMC only)

| Interface | Class            | Endpoint | Addr   | Dir    | Type    | MaxPkt | Purpose                    |
|-----------|------------------|----------|--------|--------|---------|--------|----------------------------|
| —         | —                | EP0      | `0x00` | IN/OUT | Control | 64     | Enumeration / control      |
| 0         | USBTMC `FE/03/01`| Bulk-OUT | `0x01` | OUT    | Bulk    | 64     | SCPI commands (host→dev)   |
| 0         | USBTMC `FE/03/01`| Bulk-IN  | `0x81` | IN     | Bulk    | 64     | SCPI responses (dev→host)  |

### After (USBTMC + vendor log interface)

| Interface | Class            | Endpoint | Addr   | Dir    | Type    | MaxPkt | Purpose                    |
|-----------|------------------|----------|--------|--------|---------|--------|----------------------------|
| —         | —                | EP0      | `0x00` | IN/OUT | Control | 64     | Enumeration / control      |
| 0         | USBTMC `FE/03/01`| Bulk-OUT | `0x01` | OUT    | Bulk    | 64     | SCPI commands (unchanged)  |
| 0         | USBTMC `FE/03/01`| Bulk-IN  | `0x81` | IN     | Bulk    | 64     | SCPI responses (unchanged) |
| **1**     | **Vendor `FF/00/00`** | **Bulk-IN** | **`0x82`** | **IN** | **Bulk** | **64** | **Log output (dev→host)** |

- Only additions — existing rows are identical.
- `bNumInterfaces` goes 1 → 2.
- Endpoint number `2` (IN) is free, so `0x82` is a clean, conformant pick.
- `0x82` = endpoint number 2, IN direction (bit 7); distinct from `0x81`
  (number 1), so there is no collision.
- No OUT endpoint on the log interface — logs are one-directional.
- nRF52840 USBD has ample bulk endpoints; `0x82` alongside `0x01`/`0x81` is well
  within budget.

---

## Does the USBTMC driver still work? Yes

**Linux kernel `usbtmc` driver (host):** the kernel binds drivers *per interface*.
It matches interface 0 by its class triple `FE/03/01` and claims only that
interface. Since interface 0 is unchanged, it still enumerates and still creates
`/dev/usbtmc0`; `UsbtmcKernel` needs zero changes. The vendor interface gets no
kernel driver bound (expected for class `0xFF`) and is claimed separately by the
host's `nusb` log reader. The two interfaces are independent.

**TinyUSB `usbtmc` class driver (device):** TinyUSB also dispatches per
interface. Enabling `CFG_TUD_VENDOR` and appending a vendor interface does not
touch the USBTMC class driver or its callbacks; the existing glue in
`glue/usbscpi_tinyusb.c` runs unchanged.

**Invariant that must hold:** interface 0 stays interface number 0 with its
descriptor unaltered; the vendor interface is appended as number 1 and
`bNumInterfaces` is bumped to 2. Do not renumber or reorder interface 0.

**Windows caveat:** the device is now composite. On Windows the raw-USB
(`nusb`/WinUSB) path for the *log* interface needs a WinUSB binding (WCID / MS OS
descriptor or an `.inf`). This affects only the new log interface on Windows; it
has no bearing on the USBTMC interface or the Linux `/dev/usbtmcN` path.

---

## Implementation plan (nRF52840 example)

### 1. Descriptor — `examples/nrf52840/usb_descriptors.c`

- Extend the interface enum: add `ITF_NUM_VENDOR`, so `ITF_NUM_TOTAL` becomes 2.
- Define `#define LOG_EP_IN 0x82`.
- Append a `TUD_VENDOR_DESCRIPTOR(...)` block after the USBTMC descriptors. Use
  `0x00` for the (unused) OUT endpoint and `LOG_EP_IN` for the IN endpoint.
- Grow `CONFIG_TOTAL_LEN` by `TUD_VENDOR_DESC_LEN`.
- Add a string descriptor entry for the log interface name (e.g. index 4,
  `"IoTSploit Log"`), and reference it as the vendor interface's `iInterface`.

### 2. TinyUSB config — `examples/nrf52840/tusb_config.h`

- `#define CFG_TUD_VENDOR 1`.
- `#define CFG_TUD_VENDOR_TX_BUFSIZE 512` (log ring buffer; tune to taste).
- `#define CFG_TUD_VENDOR_RX_BUFSIZE 64` (kept minimal; RX unused but some
  TinyUSB versions require a nonzero value).
- Confirm the endpoint count / FIFO settings accommodate the added bulk-IN
  (nRF52840 has room).

### 3. Firmware log backend — `examples/nrf52840/main.c`

- Route log output to the new endpoint via `tud_vendor_write()` +
  `tud_vendor_flush()`. Either:
  - register an `nrf_log` backend that forwards formatted lines, or
  - add a small `usb_log_write(const char*, size_t)` shim the code calls directly.
- **Non-blocking requirement:** logging runs alongside the SCPI path and must
  never block the main loop. `tud_vendor_write` is buffered; on overflow (host
  not draining) drop or overwrite oldest rather than spin.
- If the host is disconnected / not reading, discard silently.

### 4. Glue (optional, for reuse across examples)

- The existing `glue/usbscpi_tinyusb.c` owns only USBTMC callbacks — leave it
  alone (its header explicitly warns integrators not to redefine USBTMC
  callbacks; the vendor endpoint has separate callbacks, so no conflict).
- If the log path should be shared by pico2/esp32s3, add a small
  `glue/usblog_tinyusb.{c,h}` mirroring the existing glue style
  (a `usblog_write()` wrapper over `tud_vendor_write`/`flush`).

### 5. Host — `host/rust`

- Add a log-reader module that opens the device with `nusb`, claims interface 1,
  and loops reads on `0x82`. Mirror the device-open / interface-claim setup
  already in `host/rust/src/usbtmc_raw.rs`.
- Put it behind the existing `raw-usb` cargo feature.
- Do **not** shoehorn it into the `Transport` trait (`host/rust/src/transport.rs`) —
  that trait models the request/response command path. The log stream is a
  separate, one-directional reader.

### 6. Replicate to other examples (if they need logs)

- `examples/pico2/usb_descriptors.c` hand-rolls its descriptor — apply the same
  interface/endpoint additions.
- `examples/esp32s3` has its own `tusb_config.h` and `main/` — mirror steps 1–3.

### 7. Verify

- `lsusb -v` shows two interfaces; endpoint `0x82` present on interface 1;
  interface 0 unchanged.
- `/dev/usbtmc0` still appears; existing host SCPI commands still succeed.
- The `nusb` log reader receives bytes on `0x82`.
- Stress: flood logs while running SCPI queries — confirm the command path is
  unaffected and logging never blocks.

---

## Alternative (no new endpoint): SCPI-poll route

If an independent push stream is *not* required, you can skip endpoints entirely:
expose a `SYST:LOG?` SCPI query and have the host poll it. This reuses the
existing bulk-IN `0x81` and the already-exposed
`usbscpi_tinyusb_queue_response()` (`glue/usbscpi_tinyusb.h`). No descriptor,
host, or endpoint changes.

Trade-off: logs are **polled** and **share the command channel**, so they
interleave with responses and only arrive when the host asks. Choose Option B
when you need an independent, always-on, push-style stream; choose the SCPI-poll
route when occasional on-demand retrieval is enough.

---

## End-to-end stack (how logs reach the UI)

```
ESP32-S3 firmware log  →  ring buffer  →  usb_task  →  tud_vendor_write()  →  EP 0x82 (bulk-IN)
        │
        ▼  USB
Host: nusb second handle → claim interface 1 → read 0x82 → split lines → StreamSink
        │
        ▼  flutter_rust_bridge
Dart: UsbtmcService.deviceLogStream() → Stream<UsbtmcLogLineDto>
        │
        ▼
UI: "Device Log" tab in the Output-Log console (usbtmc_control_panel.dart)
```

The current UI reaches the device through the **raw `nusb` backend**
(`ui/rust/Cargo.toml` pulls `iotsploit-host` with `features = ["raw-usb"]`), and
every device/interface lookup matches by USBTMC class
(`ui/rust/src/api/usbtmc.rs` list filter; `host/rust/src/usbtmc_raw.rs` claim).
Adding a `0xFF` vendor interface therefore does **not** disturb enumeration,
open, or the SCPI command path — the log stream is purely additive.

---

## UI: Flutter "Device Log" component

**Where.** The console at the bottom of the Device Control panel is built by
`_buildConsole()` in
`ui/lib/screens/utils/usbtmc/usbtmc_control_panel.dart` (header "Output Log" +
`_buildLog()` list + `_buildPrompt()`). Turn that single-view console into a
**two-tab console**:

- **Tab 1 — "Output Log"** (existing): the SCPI console (`_log` / `_LogKind`,
  fed by `_emit`). Unchanged behaviour, keeps the SCPI input prompt.
- **Tab 2 — "Device Log"** (new): the firmware log streamed from EP `0x82`.
  Read-only, no prompt.

### Widget design

- Add `enum _ConsoleTab { output, device }` and `_ConsoleTab _tab` state.
- In `_buildConsole()` header row, replace the static "Output Log" title with two
  tab chips ("Output Log", "Device Log"). Keep copy/clear icons but make them act
  on the **active** tab. Add, on the Device Log tab, a **pause/resume** toggle and
  an optional level filter (All / Info / Warn / Error).
- Body becomes an `IndexedStack(index: _tab.index, children: [_buildLog(), _buildDeviceLog()])`.
- Show `_buildPrompt()` only when `_tab == output`.
- Add a small "● streaming" / "○ idle" indicator on the Device Log tab, plus an
  unread-count badge on the tab chip when it's not focused.

### New state and model (in `_UsbtmcControlPanelState`)

```dart
enum _DevLogLevel { info, warn, error, raw }

class _DevLogLine {
  _DevLogLine(this.level, this.text) : time = DateTime.now();
  final _DevLogLevel level;
  final String text;
  final DateTime time;
}

final List<_DevLogLine> _deviceLog = [];   // ring-capped, e.g. last 5000 lines
final _deviceLogScroll = ScrollController();
StreamSubscription<UsbtmcLogLineDto>? _deviceLogSub;
bool _deviceLogPaused = false;
```

### Lifecycle

- **On connect** (end of the existing connect flow, after a session id exists):
  `_deviceLogSub = UsbtmcService.deviceLogStream(sessionId).listen(_onDeviceLog);`
- `_onDeviceLog(line)`: if not paused, `setState` append to `_deviceLog` (drop
  oldest past the cap) and auto-scroll `_deviceLogScroll` to bottom; if the
  Device Log tab isn't focused, bump the unread badge.
- **On disconnect / dispose**: `await _deviceLogSub?.cancel();` and dispose
  `_deviceLogScroll`. Cancelling the Dart stream drops the Rust `StreamSink`,
  which signals the reader thread to stop (see host section).
- **Copy/Clear** on the Device Log tab operate on `_deviceLog`.

### `_buildDeviceLog()`

Mirror `_buildLog()`: a `ListView.builder` of monospace rows, colour per
`_DevLogLevel` from `context.appColors` (reuse `statusInfo` / `statusWarn` /
`statusError`), each row `SelectableText`, timestamp prefix optional. Empty state
reuses `EmptyStateWidget` ("No device output yet").

### Platform note

Direct USB is desktop-only (the panel already renders a desktop-only state on
Android/iOS), so the Device Log tab follows the same gating — on mobile it shows
the desktop-only placeholder.

---

## Host / bridge: log streaming API

### Rust bridge — `ui/rust/src/api/usbtmc.rs`

Add a streaming entry point (flutter_rust_bridge turns a `StreamSink` into a Dart
`Stream`):

```rust
pub struct UsbtmcLogLineDto { pub ts_us: u64, pub level: u8, pub text: String }

/// Stream firmware log lines from the device's vendor log interface (EP 0x82).
pub fn usbtmc_log_stream(session_id: String, sink: StreamSink<UsbtmcLogLineDto>)
    -> Result<(), String>;
```

Implementation (desktop `imp` module):

1. Look up the session's `(bus_number, device_address)` — store these in the
   session record at `open()` time (currently only the `ScpiSession` is kept).
2. Spawn a **dedicated reader thread**. It opens a **second** `nusb` `Device`
   handle by bus/address (independent of the USBTMC handle — different interface,
   so no claim conflict), claims **interface 1**, and finds the bulk-IN `0x82`.
3. Loop: submit a bulk-IN read, split the received bytes on `\n` (carry a partial
   tail between reads), strip ANSI colour, classify level from an ESP-IDF-style
   `I (…)`, `W (…)`, `E (…)` prefix, and `sink.add(UsbtmcLogLineDto{…})`.
4. **Stop conditions:** `sink.add` returns error (Dart cancelled the stream), or a
   per-session `Arc<AtomicBool>` cancel flag is set by `close()`. On stop, release
   interface 1 and drop the handle.

Keep it behind the existing desktop `#[cfg(...)]` gates; on mobile the fn returns
`Err(DESKTOP_ONLY)` / an empty stream, matching the other calls.

### iotsploit-host crate — `host/rust/src/usbtmc_raw.rs`

Add a sibling of `UsbtmcRaw` (e.g. `UsbtmcLogReader`) that:
- reuses `open_by_bus_address` to get the `Device`,
- claims the interface whose class is **`0xFF`** (vendor) instead of `0xFE/0x03`,
- exposes `read(timeout) -> Result<Vec<u8>>` over its bulk-IN endpoint.

Put it behind the same `raw-usb` feature. This keeps USB specifics in the host
crate and the bridge thin.

### Dart service — `ui/lib/services/usbtmc_service.dart`

```dart
static Stream<rust.UsbtmcLogLineDto> deviceLogStream(String sessionId) =>
    rust.usbtmcLogStream(sessionId: sessionId);
```

---

## ESP32-S3 demo: emit logs over the vendor endpoint

The demo firmware lives in `examples/esp32s3` (descriptor + callbacks are inlined
in `main/app_main.c`; config in `main/tusb_config.h`). Mirror the Option-B
descriptor change here and tee ESP-IDF logging to the endpoint.

### 1. `main/tusb_config.h`

- `#define CFG_TUD_VENDOR 1` (currently `0`).
- `#define CFG_TUD_VENDOR_TX_BUFSIZE 1024`, `#define CFG_TUD_VENDOR_RX_BUFSIZE 64`.

### 2. Descriptor (`main/app_main.c`, ~line 522)

- Enum: `enum { ITF_NUM_USBTMC = 0, ITF_NUM_VENDOR, ITF_NUM_TOTAL };`
- `#define LOG_EP_IN 0x82`.
- Append after the two USBTMC endpoint descriptors:
  `TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 4, 0x00, LOG_EP_IN, 64),`
  (string index 4 = "IoTSploit Log"; `0x00` = no OUT endpoint).
- Grow `CONFIG_TOTAL_LEN` by `TUD_VENDOR_DESC_LEN`.
- Add the interface string to the string-descriptor callback.

### 3. Route ESP-IDF logs to the endpoint (non-blocking)

ESP-IDF logging (`ESP_LOGI/W/E`) funnels through one `vprintf`-style hook. Do
**not** call TinyUSB from arbitrary task contexts — buffer instead:

- Create a FreeRTOS **ring/stream buffer** (`xStreamBufferCreate`) at init.
- Install a log hook: `esp_log_set_vprintf(&log_to_usb_vprintf);` where
  `log_to_usb_vprintf` formats into a stack buffer, writes bytes into the stream
  buffer (non-blocking; drop on full), and **also** calls the original UART
  vprintf so local serial logging still works (tee).
- In `usb_task` (already running the TinyUSB device loop, `app_main.c:610`), after
  `tud_task()`, drain the stream buffer into `tud_vendor_write()` + `tud_vendor_flush()`
  while `tud_vendor_mounted()` is true; if not mounted, discard.

### 4. Demo output to prove the path

Have the demo emit periodic lines (the wireless scan tasks already log via
`ESP_LOGx`; those now flow to the endpoint automatically). Optionally add a
1 Hz heartbeat `ESP_LOGI(TAG, "heartbeat %u", n)` in a demo task so the Device
Log tab shows continuous output even when idle.

> Note: `app_main.c` inlines the USBTMC stub callbacks and descriptors rather
> than using a shared descriptor file, so these edits are local to that file.
> The nRF52840 (`examples/nrf52840/usb_descriptors.c`) and Pico2
> (`examples/pico2/usb_descriptors.c`) need the equivalent descriptor edits if
> they should also stream logs.

---

## Compatibility recap

| Component | With the new firmware | Notes |
|---|---|---|
| CLI host (`iotsploit-host`) | ✅ Unchanged | Matches USBTMC by class; ignores vendor iface |
| UI Rust bridge / enumeration | ✅ Unchanged | `.any()` class filter still matches |
| Flutter SCPI console | ✅ Unchanged | Command path untouched |
| Flutter "Device Log" tab | 🆕 New work | Stream API + tab described above |
| Windows raw-USB | ✅ for USBTMC | Log iface needs its own WinUSB (WCID) binding |

---

## Two bugs that block the log stream (found during bring-up)

Both produce the same symptom — the device enumerates fine, `*IDN?` works, the
firmware logs to UART, but the log endpoint delivers **zero bytes** to the host.

### 1. `tud_task()` blocks under FreeRTOS → the log pump starves

With `CFG_TUSB_OS = OPT_OS_FREERTOS`, `tud_task()` is `tud_task_ext(UINT32_MAX,
false)` — it **blocks until the next USB event**. If `usb_log_pump()` is called
after `tud_task()` in the task loop, it only runs when there is USB traffic. An
idle device (no SCPI commands) never wakes the task, so buffered log lines never
reach the vendor FIFO and the host reads nothing. It *looks* intermittent: it
works during the connect burst (SCPI queries generate events) and dies once idle.

**Fix:** give the task a bounded wait so the pump runs even when idle:

```c
// examples/esp32s3/main/app_main.c  (usb_task)
tud_task_ext(10, false);   // instead of tud_task(); wakes at least every 10ms
usbscpi_task(dev);
usb_log_pump();
```

Diagnosis tip: an `esp_rom_printf` (bypasses the log hook) in the pump printing
`tud_vendor_mounted()` / `tud_vendor_write_available()` never fired at all —
proving the pump wasn't being reached.

### 2. Host `clear_halt()` on the log endpoint wedges TinyUSB's vendor TX

`UsbtmcRaw` calls `clear_halt()` on the USBTMC bulk endpoints because the kernel
usbtmc driver can leave a stale data toggle. Copying that into the log reader is
a mistake: the vendor endpoint has **no kernel driver**, so there is nothing to
clear — and sending `CLEAR_FEATURE(ENDPOINT_HALT)` to the device tears down the
in-flight IN transfer without re-arming it. The tx FIFO then fills
(`tud_vendor_write_available()` climbs to 0 and stays) and the endpoint stops
delivering until the MCU reboots.

**Fix:** `UsbtmcLogReader::from_device` must **not** `clear_halt()` the vendor
bulk-IN. A freshly opened handle already starts from a clean toggle.

Diagnosis tip: with a working endpoint, `tud_vendor_write_available()` jumps back
to the full buffer size the moment a reader attaches (FIFO draining); when
wedged, it keeps decreasing toward 0 and never recovers even while a reader is
actively polling.

## Build / verify order

1. Firmware: add vendor iface + log tee → `lsusb -v` shows iface 1 / EP `0x82`;
   USBTMC still enumerates and answers `*IDN?`.
2. Host crate: `UsbtmcLogReader` reads bytes on `0x82` (unit-test with the CLI).
3. Bridge: `usbtmc_log_stream` emits `UsbtmcLogLineDto`s; regenerate frb bindings.
4. UI: wire `deviceLogStream` into the two-tab console; confirm the Device Log
   tab fills while SCPI commands still work on the Output Log tab.
5. Stress: flood logs during workflow runs — SCPI path stays responsive, logging
   never blocks the firmware main loop.

---

## Summary

| Aspect                      | Option B (vendor iface + bulk-IN) |
|-----------------------------|-----------------------------------|
| New endpoint                | Yes — bulk-IN `0x82` on a new interface |
| USBTMC interface (0)        | Untouched, still conformant       |
| Kernel `/dev/usbtmcN`       | Works unchanged                   |
| TinyUSB USBTMC callbacks    | Unchanged                         |
| Log delivery                | Push, async, independent channel  |
| Host effort                 | `UsbtmcLogReader` (`raw-usb`) + `usbtmc_log_stream` bridge fn |
| UI effort                   | Two-tab console; "Device Log" tab bound to a frb `Stream` |
| Firmware effort             | Medium (ESP-IDF log hook → ring buffer → `tud_vendor_write`) |
| ESP32-S3 demo               | `CFG_TUD_VENDOR=1`, vendor descriptor in `app_main.c`, log tee |
| Windows note                | Log iface needs WinUSB binding (WCID/inf) |
