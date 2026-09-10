# SCPI over Ethernet and Wi-Fi

## Goal

Let a host reach an `iotsploit-usb` device over TCP — an ESP32-S3 on Wi-Fi, a
Pi 5 on Ethernet — using the same SCPI command surface, descriptor discovery,
workflows, and binary block contract it already uses over USBTMC. The only
thing that should change for a user is the `--device` string.

Non-goal: replacing USBTMC. USB stays the primary transport, the provisioning
channel, and the recovery path.

## Why the core does not change

`src/usbscpi.c` has no USB dependency, and two properties make a socket
transport nearly free:

- **`usbscpi_on_rx()` is stream-driven, not message-driven.** Commands execute
  on `\n` or `;` (`src/usbscpi.c:515`), and binary blocks are length-driven
  (`MODE_BLOCK_PAYLOAD`). The `eom` flag is read exactly once, at
  `src/usbscpi.c:585`, to flush an *unterminated* pending line. A socket can
  feed bytes as they arrive with `eom=false` and every existing command works.
  USBTMC's message boundaries were never load-bearing.
- **`usb_tx` is called synchronously from inside `on_rx`.** A socket glue calls
  `send()` inline. None of the deferred-buffering / MAV-bit /
  `tud_usbtmc_msgBulkIn_request_cb` choreography that `glue/usbscpi_tinyusb.c`
  exists to solve applies, because that whole mechanism is a workaround for
  TinyUSB's `TU_VERIFY(state == STATE_TX_REQUESTED)` guard. The network glue is
  *smaller* than the USB one.

So this is a glue-tier plus host-backend change. `src/usbscpi.c` and
`include/usbscpi/usbscpi.h` are not modified by any stage below, and the
`usbscpi_*` prefix stays — it names the component, not the bus.

## Wire protocol: raw SCPI on TCP 5025

| Option | Cost | Verdict |
| --- | --- | --- |
| VXI-11 | ONC/RPC, XDR, a portmapper | Skip |
| HiSLIP :4880 | Two TCP channels, 16-byte message headers, async SRQ | Only if NI-VISA interop is ever required |
| Raw socket :5025 | A listener and nothing else | **Chosen** |

Port 5025 is the de-facto standard for SCPI over TCP: it works with `nc`,
telnet, and pyvisa's `TCPIP::<host>::5025::SOCKET` resource string with no
device-side work beyond binding.

The existing binary contract survives raw TCP intact. `:DATA:WRITE
#<N><LEN><payload>` is self-delimiting inbound, and responses are `#<N><LEN>`
blocks followed by a terminator. Nothing needs escaping or a length prefix.

**Response terminator is a bare `\n`.** `third_party/libscpi/inc/scpi/scpi_user_config.h:18`
sets `SCPI_LINE_ENDING` to `LINE_ENDING_LF`, overriding libscpi's CRLF default.
The host framing reader in Stage 1 depends on this; if that config line ever
changes, the reader must change with it.

## The one hard part: response framing on the host

`Transport::read_msg()` (`host/rust/src/transport.rs:16`) promises "one complete
response message". USBTMC delivers exactly that — the kernel hands over a framed
message, so `UsbtmcKernel::read_msg` (`host/rust/src/usbtmc_kernel.rs:98`) can
do a single `read()` and return.

TCP makes no such promise. One `read()` returns whatever arrived in one segment:
possibly half a response, possibly a whole response plus the head of the next
one. Everything else in this plan is plumbing; this is the part that will
produce subtle, intermittent bugs if it is rushed.

Responses are self-describing, so the rule is short:

```
peek[0] == '#'  ->  read N, read LEN digits, read LEN payload bytes, then '\n'
otherwise       ->  read until '\n'
always          ->  keep the unconsumed remainder buffered for the next call
```

## The second hard part: non-query commands send nothing

libscpi writes a response terminator only when the command actually produced
output — `writeNewLine` is guarded by `if (!context->first_output)`
(`third_party/libscpi/src/parser.c:95`, called from `parser.c:250`). A non-query
command such as `GPIO:SET 2,1` produces no output, so `scpi_write_cb`
(`src/usbscpi.c:61`) is never called and **zero bytes leave the device**.

USB hides this. When the host reads, TinyUSB raises `REQUEST_DEV_DEP_MSG_IN`,
and with no buffered response the glue fabricates a one-byte `'\n'`
(`glue/usbscpi_tinyusb.c:127`) purely so the class does not get stuck in
`STATE_TX_REQUESTED`. That byte is manufactured *by the read*, not queued by the
command. A raw socket has no IN request and no equivalent, so nothing is sent at
all.

`ScpiSession::write_and_drain` (`host/rust/src/session.rs:64`) reads after every
non-query write, and workflows call it for their trigger (`workflow.rs:86`,
`:151`) and for both interactive prompt replies (`:255`, `:267`). Over TCP each
of those blocks until the read timeout, so **every workflow would time out** —
which makes the Stage 3 verification impossible as originally written.

The doc comment on `ScpiSession::write` (`session.rs:51-54`) claims "the device
still emits a `\n` terminator for non-query commands". That is false at the SCPI
layer; it describes the TinyUSB glue's dummy byte. It must be corrected with the
code, or it will justify reintroducing the drain later.

### Fix: conventional SCPI semantics

Drop the drain. A non-query command gets no response, on any transport.

This is safe on USB precisely because the dummy byte is generated on demand
rather than queued: `write(cmd)` followed later by `query(q)` still reads `q`'s
real response, since the glue only has `q`'s reply buffered when the IN request
arrives.

It relies on one contract: **a command registered as non-query must not call
`SCPI_Result*`.** If one does, its output stays buffered and the next query
reads it instead — a desync the drain currently masks. That is a device-side bug
either way, and hiding it is not a reason to keep the drain.

The alternative, defining a transport-independent acknowledgement, is a protocol
change touching every device and both hosts. Not worth it to preserve a call
that only ever consumed a filler byte.

---

## Dual transport on one device: two contexts, not one

This is the constraint that shapes Stage 4, and an earlier draft of this plan
got it wrong by proposing a single shared `usbscpi_t` guarded by a mutex.

**A context can only reply on one transport.** `usbscpi_config_t` holds exactly
one `usb_tx` callback (`include/usbscpi/usbscpi.h:106`), `usbscpi_init` requires
it to be non-NULL (`src/usbscpi.c:441`), and *every* response goes through it
(`src/usbscpi.c:66`). The ESP32 example binds it permanently to the TinyUSB
sender (`examples/esp32s3/main/app_main.c:39`, `:692`). A command arriving over
TCP on that context would send its reply out over USB. Pointing the callback at
the socket instead would break USB. No amount of locking fixes response routing
— a mutex serialises access, it does not choose a destination.

The parser state is the same story: one `line_buf`, one error queue, one
`MODE_BLOCK_*` machine per context. A TCP `recv()` delivering half a command
leaves residue that a subsequent USB message would append to. Serialising whole
`on_rx` calls does not prevent interleaving at *command* granularity.

**Use two contexts.** One for USB, one for TCP, each with its own storage,
`line_buf`, `io_buf`, error queue, and `usb_tx`. They register the same command
tables and their callbacks reach the same underlying device services.

- Cost on ESP32-S3: the current context uses `s_storage[2048]`, `s_line[96]`,
  `s_io[4096]` (`app_main.c:21-23`) — roughly 6.2 KB for a second set. Negligible
  against the part's SRAM.
- No core change. `usbscpi_sizeof()` / `usbscpi_init()` already support multiple
  instances; nothing in `src/usbscpi.c` assumes a singleton.
- `usbscpi_tinyusb_bind()` keeps its own global and drives the USB context;
  `usbscpi_socket_serve(ctx, ...)` already takes its context as a parameter, so
  the Stage 2 API needs no change.
- Per-session error queues are correct instrument behaviour, not a regression: a
  TCP client should not consume errors raised by the USB session.

What genuinely remains shared is **device service state** — the BLE scan result
statics, GPIO, the radio. That is a narrower problem than parser sharing, and it
already exists today between the SCPI task and the BLE event callbacks. Handle it
where it lives, at the service, not by adding arbitration to the core.

Still true, and cheap:

- **One TCP client at a time**, by construction — `usbscpi_socket_serve` handles
  one connection per loop iteration; a second waits in the listen backlog.
- **`usbscpi_clear()` on every disconnect**, so a client that dies mid-block
  cannot poison the next session on that context.

---

## Stage 1 — Host: framing reader, `TcpTransport`, and the drain fix

**Files:** `host/rust/src/tcp.rs` (new), `host/rust/src/lib.rs`,
`host/rust/Cargo.toml`, `host/rust/src/session.rs`, `host/rust/src/workflow.rs`,
`host/rust/src/bin/iotsploit-host.rs`

Add a `tcp` feature. No new dependencies — `std::net::TcpStream` is enough,
which keeps the crate's dependency-light rule stated in `Cargo.toml`.

```toml
[features]
default = ["kernel"]
tcp = []
```

### The reader is generic over `Read`, and lives in `tcp.rs`

```rust
/// Splits a SCPI byte stream into response messages.
struct MsgReader<R: Read> {
    inner: R,
    buf: Vec<u8>,   // read but not yet consumed
}

impl<R: Read> MsgReader<R> {
    /// Read until one complete message is available; return it and retain
    /// any trailing bytes for the next call.
    fn read_msg(&mut self, max_len: usize) -> Result<Vec<u8>>;
}
```

Being generic over `Read` rather than owning a `TcpStream` is what makes it
testable without a socket, and it makes `TcpTransport` a thin wrapper.

**Do not add `host/rust/src/scpi_reader.rs` as a shared module.** TCP is the
only consumer, and a second one (serial) does not exist. Per the repo's
"search and reuse first / three similar lines beat a helper nobody else calls"
principle, it stays private to `tcp.rs` until something else actually needs it.

### Frame-splitting cases the implementation must handle

Working on the front of `buf`, returning "need more bytes" rather than
erroring whenever the buffer is merely short:

| Condition | Action |
| --- | --- |
| `buf` empty | need more |
| `buf[0] != b'#'` | scan for `\n`; if absent, need more |
| `buf[0] == b'#'`, `len < 2` | need more |
| `buf[1]` not in `b'1'..=b'9'` | `Error::Scpi` — indefinite-length `#0` is never emitted by this firmware |
| `len < 2 + n` | need more |
| length digits not all ASCII digits | `Error::Scpi` |
| declared `len > max_len` | `Error::Scpi` — refuse before allocating |
| `len < 2 + n + declared + 1` | need more |
| byte at `2 + n + declared` is not `\n` | `Error::Scpi` — desynchronised stream |
| no `\n` and `buf.len() > max_len` | `Error::Scpi` — runaway response, don't grow forever |
| underlying `read()` returns 0 | `Error::Device("connection closed by device")` |

The last three matter most: without them a desynchronised or hostile stream
grows the buffer without bound.

### `TcpTransport`

```rust
pub struct TcpTransport { r: MsgReader<TcpStream>, w: TcpStream }

impl TcpTransport {
    pub fn connect(addr: &str, timeout: Duration) -> Result<Self>;
}
```

- `set_nodelay(true)`. Without it Nagle adds roughly 40 ms to every query
  response, because each SCPI exchange is a small write followed by a wait.
- `set_read_timeout(Some(timeout))`; map `WouldBlock` and `TimedOut` to the
  existing `Error::Timeout` so a wedged network device behaves like a USB
  device that gets `-110` from the kernel.
- Clone the stream (`try_clone`) so the reader and writer halves are separate,
  or keep one stream and borrow — either is fine; the split just avoids
  borrow friction inside `read_msg`.

### Tests

Every host module already carries `#[cfg(test)]` unit tests
(`block.rs:129`, `session.rs:147`, `caps.rs:98`, and others), so this follows
existing convention rather than expanding scope. The framing reader is exactly
the "uncovered, high-risk regression path" the testing policy carves out: pure
logic, many boundary cases, and failures that are intermittent in the field.

Drive it with a scripted chunking reader, no sockets:

```rust
struct Chunks(VecDeque<Vec<u8>>);   // impl Read, returns one scripted slice per call
```

Cases worth covering: a text response split mid-line; a block split inside its
length digits; a block split inside its payload; two responses in one chunk;
a response whose payload contains `\n` and `#`; declared length over `max_len`;
EOF mid-message.

### Remove the drain-after-write assumption

Blocker for Stage 3; the reasoning is in "The second hard part" above. Six call
sites plus two doc comments:

| Location | Change |
| --- | --- |
| `session.rs:64` | Delete `write_and_drain` — it has no correct use over TCP |
| `session.rs:51-54` | Correct the `write` doc comment: the device does *not* emit a terminator for non-query commands |
| `workflow.rs:86`, `:151` | `write_and_drain(&trigger)` → `write(&trigger)` |
| `workflow.rs:255`, `:267` | Same, for the two interactive prompt replies |
| `workflow.rs:75`, `:140` | Update the step lists that name `write_and_drain` |
| `bin/iotsploit-host.rs:349`, `:496` | Same, for the `write` subcommand and the REPL |

Delete the method rather than leaving it unused — Delete > Replace > Add, and an
unused helper invites someone to reach for it again.

**Verify:** `cargo test --features tcp` in `host/rust/`, and by hand against
`nc -l 5025` echoing a canned response. Re-run an existing workflow over USB to
confirm the drain removal did not regress the USB path.

---

## Stage 2 — Device: socket glue and a Linux target

**Files:** `glue/usbscpi_socket.c`, `glue/usbscpi_socket.h`,
`examples/linux/main.c`, `CMakeLists.txt`

### The glue

Two functions, mirroring how `glue/usbscpi_tinyusb.h` presents its surface:

```c
/* Use as usbscpi_config_t.usb_tx. Sends inline; no buffering. */
int usbscpi_socket_tx(void *user, const uint8_t *data, size_t len, bool eom);

/* Bind, listen, and serve clients one at a time. Blocks; run it in its own
 * task (FreeRTOS) or thread (Linux). Returns only on unrecoverable error. */
int usbscpi_socket_serve(usbscpi_t *ctx, const char *bind_addr, uint16_t port);
```

Portability is a two-line include guard — lwIP's socket API is BSD-shaped:

```c
#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif
```

Loop body: `accept()` → set `TCP_NODELAY` → `recv()` into a stack buffer →
`usbscpi_on_rx(ctx, buf, n, false)` → repeat until `recv()` returns 0 or a
negative → `usbscpi_clear(ctx)` → `close()` → back to `accept()`.

`usbscpi_clear()` on disconnect is not optional. Without it, a client that dies
mid-block leaves `MODE_BLOCK_PAYLOAD` and a partial line buffer in place, and
the *next* session inherits the corruption.

`eom=false` always. The socket has no message boundary to report, and
`src/usbscpi.c:585` only uses `eom` to flush an unterminated line — which over
TCP would mean executing a half-received command.

**Binding:** `bind_addr` is a required argument, not defaulted to `0.0.0.0`.
See Stage 6.

### `examples/linux/`

A small `main.c` that allocates `usbscpi_storage`, registers two or three demo
commands, and calls `usbscpi_socket_serve(ctx, "127.0.0.1", 5025)`.

This is the highest-leverage part of the whole plan and the reason Stage 2
precedes any ESP32 work. **The Pi 5 is a Linux host, not an MCU**, so the
"device" side of a Pi 5 deployment *is* this daemon — the Pi 5 target is not
extra work, it falls out of the example. And it means the entire SCPI device
runs on `localhost:5025` with no hardware attached, which makes the Rust host,
descriptor discovery, and workflow execution testable end to end on a laptop.

`examples/linux/` does not duplicate the TinyUSB plumbing that
`docs/refactor-examples-to-core.md` and `docs/consolidate-examples-into-core-plan.md`
are trying to eliminate, so it does not add to the Tier 1 / Tier 2 duplication
those plans target. It is still worth confirming ordering with whoever owns
those plans before adding a new `examples/` directory.

### CMake

Follow the existing option pattern at `CMakeLists.txt:18`:

```cmake
option(USBSCPI_BUILD_SOCKET_GLUE "Build iotsploit-usb optional BSD-socket glue" OFF)
```

with a matching `add_library(usbscpi_socket_glue STATIC glue/usbscpi_socket.c)`
linking `usbscpi`, alongside the existing `usbscpi_tinyusb_glue` block. The
ESP-IDF branch at the top of the file returns early and is unaffected; the
ESP32 build picks the glue up through the component's `SRCS` in Stage 4.

**Verify:**

```sh
cmake -S . -B build -DUSBSCPI_BUILD_SOCKET_GLUE=ON
cmake --build build
./build/examples/linux/usbscpi_linux &
printf '*IDN?\n' | nc 127.0.0.1 5025
```

---

## Stage 3 — Wire the backend into the CLI

**Files:** `host/rust/src/bin/iotsploit-host.rs`, `host/rust/src/lib.rs`

Add `Backend::Tcp` to the enum at `iotsploit-host.rs:164` and its two arms to
the `impl Transport for Backend` block.

### `--device` becomes a URI

Today `Cli.device` is a `PathBuf` (`iotsploit-host.rs:78`). Change it to
`String` and resolve by scheme in `open_backend`:

| `--device` value | Backend |
| --- | --- |
| `tcp://192.168.4.1:5025` | `Tcp` |
| `usb://2e8a:000a` | `Raw` |
| anything else | `Kernel`, via `PathBuf::from` |

One flag covers every transport, and it is the natural extension point for
whatever comes next. Add `tcp` to the `--backend` parser at
`iotsploit-host.rs:111` for the explicit case. A `--device` with a scheme wins
over `--backend`; without one, the existing `Auto` order (kernel, then raw) is
unchanged, and TCP is never auto-detected — there is nothing to enumerate.

`open_kernel` gains a `PathBuf::from(s)`; `list` still enumerates
`/dev/usbtmc*` only and needs no change.

### Two things deliberately cut from the earlier proposal

- **No `xport=` field in `SYSTem:CAPabilities?`.** The host already knows how it
  connected. Adding it would mean editing `cmd_syst_cap` in `src/usbscpi.c:110`
  to serve a value nothing consumes — it breaks the "core is untouched"
  property for no benefit.
- **No new MTU plumbing.** `cfg.mtu` already caps `SYSTem:HELP:HEADers?`
  (`src/usbscpi.c:133`) and block reads (`src/usbscpi.c:194`). A socket device
  just passes a larger `.mtu` in its own `usbscpi_config_t`. That is a value in
  the example, not a code change anywhere.

**Verify:** `iotsploit-host --device tcp://127.0.0.1:5025 idn` and
`... profile` against the Stage 2 daemon. Descriptor discovery and at least one
workflow must round-trip identically to USB.

The workflow half of that check depends on the Stage 1 drain removal. Without
it, the trigger's `write_and_drain` blocks until the read timeout and every
workflow fails — the daemon is correct and the host is not.

---

## Stage 4 — ESP32-S3 SoftAP

**Files:** `examples/esp32s3/main/net_scpi.c`, `examples/esp32s3/main/app_main.c`,
`examples/esp32s3/main/CMakeLists.txt`

SoftAP is the default topology: the ESP32 *is* the access point, the host joins
its SSID and reaches it at a fixed `192.168.4.1:5025`. One hop, no DHCP hunt, no
discovery mechanism, and no dependency on any existing network. STA mode (Stage
5) is the bench convenience, not the baseline.

- `esp_netif_create_default_wifi_ap()` alongside the existing
  `esp_netif_create_default_wifi_sta()` at `wifi_scan.c:46`.
- `esp_wifi_set_mode(WIFI_MODE_APSTA)` — `wifi_scan.c:55` currently sets
  `WIFI_MODE_STA`. APSTA keeps the STA interface that `WIFI:SCAN` needs.
- A FreeRTOS task (stack ≥ 4096) running `usbscpi_socket_serve(ctx, "0.0.0.0", 5025)`.
  On SoftAP the device owns the subnet, so binding to the AP interface is the
  intent; see Stage 6 for making that explicit rather than incidental.
- **A second `usbscpi_t` for the socket**, with its own storage, `line_buf`,
  `io_buf`, and `usb_tx` pointing at `usbscpi_socket_tx`. The USB context keeps
  its TinyUSB binding untouched. Both register the same command tables. See
  "Dual transport on one device" above for why sharing one context cannot work.

**Verify on hardware:** join the SSID, then
`iotsploit-host --device tcp://192.168.4.1:5025 profile`, and run the BLE scan
workflow to completion.

---

## Stage 5 — STA mode and provisioning over USB

**Files:** `examples/esp32s3/main/wifi_scan.c` (or a new `wifi_sta.c`),
`examples/esp32s3/main/app_main.c`

Wi-Fi is not provisioned today. `wifi_scan.c` calls
`esp_netif_create_default_wifi_sta()` and `esp_wifi_start()` but **never calls
`esp_wifi_connect()`** — STA mode exists there only to enable scanning. There
is no association, no DHCP lease, and no IP address.

That creates a chicken-and-egg: credentials cannot arrive over a link that
credentials are needed to establish. **Provision over USBTMC**, which is already
connected:

```
WIFI:STA:SSID "myssid"
WIFI:STA:PASSword "secret"
WIFI:STA:CONNect
WIFI:STA:IP?          -> 192.168.1.57
```

Persist to NVS, which `wifi_scan.c:35` already initializes. Then add mDNS
(`_scpi-raw._tcp`) so the host can find the device without reading the IP off a
serial console; ESP-IDF ships an `mdns` component and the Pi 5 has Avahi.

---

## Stage 6 — Access control

**Files:** `glue/usbscpi_socket.c`, `glue/usbscpi_socket.h`

Over USB, physical access gates the command surface. Over Wi-Fi, the device
offers GPIO control, BLE attack workflows, and `:DATA:WRITE` to anyone on the
LAN, unauthenticated. For a tool named *iotsploit* that is a real change in
exposure, not a theoretical one.

- Network transport off by default: no listener unless the app calls
  `usbscpi_socket_serve` explicitly.
- `bind_addr` is a required parameter with no default, so choosing `0.0.0.0` is
  a visible decision in the calling code.
- A pre-shared token, **enforced in the glue, not the core**: until a session
  sends `SYSTem:AUTH <token>`, the glue answers from its own buffer and never
  calls `usbscpi_on_rx` at all. `*IDN?` is the one exception, so discovery still
  works. This keeps `src/usbscpi.c` untouched — an earlier draft of this plan
  proposed gating inside `feed_line`, which would have put transport policy in
  the core and broken the property the whole design rests on.
- TLS via `esp-tls` / mbedTLS is the real answer and is out of scope here.

---

## Open decision: `WIFI:SCAN` destroys its own transport

This needs an answer before Stage 4, not after.

`examples/esp32s3/main/wifi_scan.c` exposes a scan workflow, and
`esp_wifi_scan_start()` with an all-zero `wifi_scan_config_t`
(`wifi_scan.c:63`) sweeps every channel. That takes the radio off-channel: in
STA mode it stalls or drops the association, and in SoftAP mode it disconnects
the client. Over USB this was free, because the physical link is independent of
the radio. Over Wi-Fi, **the command travels on the link it breaks**.

| Option | Cost |
| --- | --- |
| **Refuse when the session is a socket** — return `-221,"Settings conflict"` | Smallest correct behaviour; no host changes; scan stays available over USB, which is where it is most useful anyway |
| Restrict the scan to the current channel | Changes what the feature does; results become misleading |
| Accept the drop; host auto-reconnects and resumes | Most user-friendly, most code: reconnect logic, workflow resumption, and a way to retrieve results gathered while disconnected |

**Recommendation: refuse.** It is a few lines, it is honest about the hardware
constraint, and the USB path already covers the use case. The auto-reconnect
option is a real feature and should be its own plan if it is wanted.

---

## Sequence and file summary

| Stage | New | Modified |
| --- | --- | --- |
| 1 | `host/rust/src/tcp.rs` | `host/rust/src/{lib.rs,session.rs,workflow.rs}`, `bin/iotsploit-host.rs`, `Cargo.toml` |
| 2 | `glue/usbscpi_socket.{c,h}`, `examples/linux/main.c` | `CMakeLists.txt` |
| 3 | — | `host/rust/src/bin/iotsploit-host.rs` |
| 4 | `examples/esp32s3/main/net_scpi.c` (2nd context + AP + task) | `examples/esp32s3/main/{app_main.c,wifi_scan.c,CMakeLists.txt}` |
| 5 | — | `examples/esp32s3/main/wifi_scan.c`, `app_main.c` |
| 6 | — | `glue/usbscpi_socket.{c,h}` |

`src/usbscpi.c` and `include/usbscpi/usbscpi.h` appear in neither column.

Stages 1–3 are roughly a day and stand on their own: they produce a
hardware-free SCPI device on `localhost:5025` and a host that talks to it, which
is worth having as a test rig whether or not the Wi-Fi feature ships. Stages 4–6
need the board.
