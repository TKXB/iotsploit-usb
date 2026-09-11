# Continuous acquisition: a dedicated data plane

## Goal

Carry a continuous record stream off the device without disturbing the SCPI
command surface. SCPI stays the control plane on both transports; a second
channel carries records, device-to-host, unsolicited.

**The data plane is protocol-agnostic.** It moves opaque, fixed-stride records
from a ring buffer to a transport and never interprets them. SocketCAN is the
first consumer and lives in Annex A; nothing in the core design may assume it.
An ADC capture, a logic-analyser trace, or an SDR tap must be able to use the
same glue with no change to it.

## Decision

**Pattern C — separate data channel. No USB488 SRQ.**

SCPI is request/response by construction; nothing in IEEE 488.2 lets a device
send bytes that were not asked for. The three ways around that:

| | Mechanism | Verdict |
| --- | --- | --- |
| A | Poll and drain a device-side buffer (`DATA:COUNt?` + `DATA:READ?`) | Works, already implemented, but every drain costs a round trip and latency is the poll interval |
| B | SRQ notification, then drain | Tells the host *when* to do A. Needs an interrupt endpoint, a caller, and host support that do not exist |
| C | **Dedicated channel, device pushes** | **Chosen** |

B is rejected on a specific ground: SRQ answers "when should I poll?", and a
push channel deletes the question rather than answering it.

A is not deleted. `DATA:COUNt?`/`DATA:READ?` remain the right tool for bounded,
on-demand reads, and the MTU bug below is worth fixing regardless.

## Current state, verified

- `src/usbscpi.c` has no platform headers; the core needs no changes.
- `glue/usbscpi_socket.c` carries the glibc / lwIP / Winsock platform block as
  **file-local statics**.
- `helpers/ring_buffer.c` is **used only by `tests/test_usbscpi.c`**. No shipped
  example uses it; it has never run cross-thread.
- `glue/usbscpi_tinyusb.c:38` defines `usbscpi_tinyusb_set_srq()`; nothing calls
  it, and no example declares an interrupt endpoint.
- `examples/esp32s3/main/app_main.c:557` is already composite: USBTMC on itf 0
  (`0x01`/`0x81`) plus a vendor interface on itf 1 (`0x02`/`0x82`) pushing
  ESP-IDF logs. `usb_log_pump()` (`:642`) is already a non-blocking drain.
- `host/rust/src/usbtmc_raw.rs:493` reads that vendor endpoint as **raw bytes**,
  with no framing and no version negotiation.
- `CMakeLists.txt:28` pins non-MSVC builds to `c_std_99`.

## Architecture

```
                 control plane                 data plane
TCP     SCPI :5025  request/response     DATA :5026  records, push
USB     USBTMC itf 0  bulk 0x01/0x81     vendor itf  bulk-IN
                 └──── usbscpi_stream_serve(ring, …, stride) ────┘
```

## The contract

```c
/* glue/usbscpi_stream.h — transport tier, payload-agnostic */

/* Blocks. Accepts one consumer at a time and drains `ring` to it until the
 * consumer disconnects. Run it in its own thread.
 *
 * `stride` is the record size in bytes, or 1 for an unstructured byte stream.
 * The glue never interprets record contents; it uses `stride` only to keep
 * record alignment across a reconnect (see "Torn records", below).
 *
 * Returns non-zero only if the listener could not be created or bound. */
int usbscpi_stream_serve(usbscpi_ring_t *ring, const char *bind_addr,
                         uint16_t port, size_t stride);

/* Non-zero while a consumer is attached. Lets the producer skip encoding
 * records nobody will read — see "Unattached is not a gap". */
int usbscpi_stream_attached(void);

/* Records discarded by the *glue* to restore alignment after a consumer
 * vanished mid-record. Monotonic, 64-bit. This is the only loss the glue
 * can cause; ring-overflow loss belongs to the producer and is counted
 * there. */
uint64_t usbscpi_stream_torn(void);
```

`usbscpi_stream_serve()` blocks, mirroring `usbscpi_socket_serve()`. The
application owns the thread, so the component needs no portable threading
abstraction.

**Counters are owned where the event happens.** The glue can only lose data one
way — a torn record on disconnect — and counts exactly that. Ring-overflow loss
is detected by the producer, so the producer owns that counter and reports it
through SCPI. There is deliberately no glue-side API for the producer to report
into, because that would be a counter the glue cannot maintain.

## Design rules

These came out of reading the code and reviewing the first draft. Each one is a
mistake a direct implementation would otherwise make.

### 1. A reconnect must not destroy record alignment

The drain sends whatever `send()` accepts and advances the ring by that many
bytes. If a partial record is sent and the consumer then disappears, the ring's
tail sits mid-record and **the next consumer starts misaligned** — permanently,
because the glue cannot see record boundaries.

This is why `stride` is in the signature. On disconnect the glue advances the
tail forward to the next `stride` boundary, discarding at most one partial
record and incrementing `usbscpi_stream_torn()`. With `stride == 1` there is
nothing to align and the counter never moves.

Advancing the tail is consumer-side only, so it is safe against a concurrently
writing producer. **Do not call `usbscpi_ring_clear()`** — it writes `head` too,
which is the producer's, and races.

*Test:* kill a consumer mid-record, reconnect, assert the first byte received is
a record boundary and `torn` incremented by exactly one.

### 2. The drain loop needs a real event loop

"Stop draining this round" is not an implementation. Without a defined wait, a
direct reading either busy-spins or invents an arbitrary sleep, and neither
notices a FIN while idle.

Specify it:

- Always `poll()`/`select()` on the consumer socket for `POLLIN|POLLHUP` — even
  with nothing to send — so a disconnect is noticed while idle rather than at
  the next record.
- Add `POLLOUT` only while a send returned `EWOULDBLOCK`/`WSAEWOULDBLOCK`.
- Use a bounded timeout (start at 5 ms) so an empty ring costs one wakeup per
  timeout rather than a spin. The timeout is the worst-case added latency, and
  it is the knob to turn if a consumer needs tighter.

A producer-driven wake (eventfd, self-pipe) would remove the timeout but needs a
per-platform primitive inside the component — rejected for now, for the same
reason threads are.

*Test:* idle CPU below a threshold with no producer; disconnect detected within
one timeout while idle.

### 3. Loss must be countable, end to end

"Gaps are reported, never inferred" only holds if every place data can vanish
has a counter wide enough not to wrap:

- **Glue-side:** `usbscpi_stream_torn()`, 64-bit.
- **Producer-side:** ring-overflow drops, 64-bit monotonic **total** — not a
  per-record delta. A narrow delta wraps and silently under-reports.
- **Source-side:** whatever the data source itself can drop before the producer
  ever sees it. This is transport-specific and belongs to the consumer; the core
  requirement is only that it be counted and exposed. Annex A gives the
  SocketCAN case.

Each record should carry the producer's running total at capture time so a
consumer sees the gap in-band, at the point it occurred.

### 4. Unattached is not a gap

`usbscpi_stream_attached()` invites the producer to skip work when nobody is
reading. Whether skipped samples count as drops is a **semantic decision that
must be written down**, not left to the implementation:

> Records not produced because no consumer was attached are **not** drops. The
> drop counter measures loss from a running capture, not time spent idle.

Consumers therefore must not treat a counter jump across an attach as loss. If a
consumer needs continuity across reconnects, it must keep the capture running
and accept ring overflow as the (counted) cost.

### 5. `usbscpi_ring_write()` writes partially

`helpers/ring_buffer.c` truncates to available space and returns the count:

```c
size_t free_bytes = usbscpi_ring_free(ring);
if (len > free_bytes) { len = free_bytes; }
```

For a fixed-stride stream that is **worse than dropping** — a truncated record
desynchronises everything after it. Producers must check capacity first and drop
whole records:

```c
if (usbscpi_ring_free(&ring) < stride) { dropped++; return; }
usbscpi_ring_write(&ring, rec, stride);
```

Do **not** change the ring helper. Partial write is correct for a byte stream,
which is what its existing test asserts; record atomicity is the caller's job.

### 6. Memory ordering, without a C-standard bump

`head` and `tail` are plain `volatile size_t`. `volatile` stops the *compiler*
reordering; it does not stop the *CPU*. This design puts the ring across a
thread boundary for the first time, on aarch64 (Pi 5) and potentially Xtensa
dual-core (ESP32-S3) — both weakly ordered. The producer's `head += len` can
become visible before the payload it publishes.

The obvious prescription, "require C11 atomics", is the wrong fix here: MSVC's
`<stdatomic.h>` is recent and gated behind `/experimental:c11atomics`, and the
Windows build path is new. Use compiler builtins instead, which work in C99:

- GCC/Clang (Linux, MinGW, ESP-IDF, every embedded target): `__atomic_load_n` /
  `__atomic_store_n` with `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE`.
- MSVC: interlocked intrinsics plus `_ReadWriteBarrier`/`MemoryBarrier`.
- Neither: `#error`. **There is no `volatile` fallback** — a silent fallback
  reintroduces exactly the bug being fixed.

`CMakeLists.txt` keeps `c_std_99`; nothing needs raising.

`usbscpi_stream_attached()` is also cross-thread state and gets the same
treatment. So does the producer's drop counter if SCPI reads it from the control
thread.

### 7. `DATA:READ?` loses data on an MTU failure

`src/usbscpi.c:190` calls `data_read()`, which **consumes** from the source.
`:194–203` then checks the MTU and returns `SCPI_RES_ERR` without putting
anything back. Ask for more than the MTU allows and the readings are gone.

Fix by checking before consuming: compute the largest `to_read` whose encoded
block fits `cfg.mtu`, clamp, and only then call `data_read()`. An oversized
request returns a short block instead of an error and a hole — which is what a
draining caller wants anyway.

Independent of everything else here; land it on its own.

## Stages

Each stage ends green. Nothing proceeds on a red build.

### Stage 1 — lift the platform shims

`glue/usbscpi_socket.c`'s platform block becomes `glue/usbscpi_sock_compat.h`:
`usbscpi_sock_t`, `USBSCPI_SOCK_INVALID`, `usbscpi_closesocket`,
`sock_interrupted()`, `sock_startup()`, `sock_cleanup()`, and the
`_WIN32_WINNT` raise (which must stay above every include — MinGW's CRT headers
latch it via `<stdint.h>`). Statics become `static inline`. No behaviour change.

*Verify:* Linux build + `ctest` 3/3; MinGW cross-build clean; ESP-IDF build of
`examples/esp32s3` clean; `tests/scpi_tcp_smoke.py` 23/23 on Linux and Windows.

### Stage 2 — atomics in the ring

Rule 6, on its own, before any threaded user exists. Add the builtin
abstraction, convert `head`/`tail`, keep `c_std_99`.

*Verify:* existing `ctest` unchanged; MinGW and ESP-IDF builds clean; a
producer/consumer stress test under ThreadSanitizer on Linux.

### Stage 3 — stream glue with a synthetic producer

`glue/usbscpi_stream.c` under the existing `USBSCPI_BUILD_SOCKET_GLUE` option.
Listener, one consumer, zero-copy drain via `usbscpi_ring_peek_linear()` +
`usbscpi_ring_advance()`. Implements rules 1 and 2 in full.

Producer is a generator of known fixed-stride records — **no CAN, no hardware**.
This is what keeps the glue honest about being protocol-agnostic.

*Verify:* a Python consumer asserting record count, stride alignment, and
monotonic in-band sequence. Plus the three cases a naive implementation fails:
consumer stalled (backpressure, producer never blocks), consumer killed
mid-record then reconnecting (rule 1, `torn == 1`, alignment restored), and idle
with no producer (CPU floor, FIN detected within one timeout).

### Stage 4 — fix `DATA:READ?` MTU ordering

Rule 7. Add a regression case for an oversized request returning a short block
with the source intact.

*Verify:* `ctest`.

### Stage 5 — first consumer

Annex A. Only now does any CAN-specific code exist.

### Stage 6 — host-side data-plane client

`host/rust/`: query the port and record format over SCPI, validate stride,
connect, parse, and report gaps from the in-band counters rather than inferring
them.

*Verify:* end-to-end against Stage 5.

## Annex A — first consumer: SocketCAN

**Example-level. Nothing here belongs in the glue.** It is written down so the
generic rules above have a worked case, and so the loss-accounting requirement
in rule 3 has a concrete answer for this source.

A CAN record needs a timestamp, an identifier, a length, flags, a payload, and
the producer's running drop total. Sizing the payload for CAN FD (64 bytes)
rather than classic CAN avoids a format migration later; the cost is wasted
bytes on classic frames, which is negligible against either transport's
bandwidth. The record total must be a power-of-two-friendly stride that the
producer and the host agree on via `SYSTem:STReam:FORMat?`.

Source-side loss for SocketCAN specifically:

- **Kernel receive-queue overflow is invisible to `read()`.** Use `recvmsg()`
  with `SO_RXQ_OVFL` to get the kernel's own drop count, and fold it into the
  reported total. Without this, frames vanish between the controller and the
  producer with nothing counted — which would break rule 3's promise.
- **Timestamps.** `SO_TIMESTAMPING` gives arrival time; a `clock_gettime()` in
  the producer gives userspace dequeue time, which is later and jittery under
  load. If the field is documented as arrival time, it must come from the
  kernel.
- Enable `CAN_RAW_FD_FRAMES`; enable error frames via `CAN_ERR_MASK` so bus-off
  and error-passive appear as records rather than as silence.

Control surface (the `SYSTem:STReam:*` half is generic; the `CAN:*` half is not):

| Command | Kind | Purpose |
| --- | --- | --- |
| `SYSTem:STReam:PORT?` | query | Data-plane port; `0` when absent |
| `SYSTem:STReam:STARt` / `:STOP` | command | Start/stop filling the ring |
| `SYSTem:STReam:FORMat?` | query | Record version and stride |
| `SYSTem:STReam:DROPped?` | query | Producer + source drop total |
| `SYSTem:STReam:TORN?` | query | Glue-side realignment discards |
| `CAN:OPEN "can0"` / `CAN:CLOSe` | command | Bind / release |
| `CAN:FILTer:ADD <id>,<mask>` | command | `setsockopt(CAN_RAW_FILTER)` |
| `CAN:SEND <id>,<hex>` | command | Single frame; bulk TX stays on `:DATA:WRITE` |

Interface bring-up stays out: `ip link set can0 type can bitrate 500000` is the
operator's job, not the SCPI surface's.

*Verify:* `vcan0` driven by `cangen`, compared against `candump` for the same
window. A virtual interface exercises everything except real bus timing and
error frames — confirm on real hardware before calling it done.

## Annex B — USB binding: Option B, multiplexed

Not in scope until firmware with a real source exists. Recorded so Stage 3's API
does not have to guess.

The USBTMC bulk-IN at `0x81` **cannot** be reused: every transfer there is a
USBTMC message with a `bTag` header, emitted only after
`REQUEST_DEV_DEP_MSG_IN`, and TinyUSB enforces it with
`TU_VERIFY(state == STATE_TX_REQUESTED)`.

**Chosen: multiplex records onto the existing vendor interface** (itf 1,
`0x02`/`0x82`) behind a record-type tag, rather than adding a third interface.
No new endpoints — the ESP32-S3 already uses four, and `TUD_VENDOR_DESCRIPTOR`
would force an OUT/IN pair for a third even though only IN is wanted
(`app_main.c:560`). It also reuses `usb_log_pump()` rather than standing up a
second drain beside it.

Envelope, since the pipe carries unframed text today and multiplexing requires
framing both kinds:

```
[u8 type][u8 rsv][u16 len]  payload        /* little-endian len */

type 0x01  log text   len = byte count, need not be a whole line
type 0x02  record     len = stride
```

**Records win under pressure.** Logs are diagnostics; records are the
measurement. The pump fills records first, spends the remaining
`tud_vendor_write_available()` on log bytes, and drops log bytes — never
records — when space is short. Cap log bytes per round so a chatty `ESP_LOGx`
cannot starve the data path.

### Legacy compatibility must be device-side, not host-side

`host/rust/src/usbtmc_raw.rs:493` reads `0x82` as raw bytes with no negotiation.
A host-side "query the format before claiming" rule only protects a **new** host
against **old** firmware. The dangerous direction is the reverse: an existing
host meeting new firmware would read framed bytes as text and garble.

So the device must default to legacy raw mode:

- Framing is **off at boot**. The vendor pipe emits raw log text exactly as
  today.
- Framing is enabled only by an explicit SCPI command from a host that
  understands it. An old host never sends it, so an old host never sees a frame.
- Records cannot flow until framing is on, which is consistent: they only flow
  after `SYSTem:STReam:STARt` anyway.

The alternative — a separately identifiable interface or PID for the framed
protocol — is heavier and only worth it if opt-in proves insufficient.

Migration once framing is enabled: the device wraps log bytes in the envelope;
the host demultiplexes and emits log payloads into the *same* `StreamSink`, so
the Flutter UI contract is unchanged and the break stays contained to the Rust
backend.

Separately: a vendor interface has no Windows class driver and needs MS OS 2.0
descriptors for WinUSB association, or Zadig. Pre-existing
(`docs/add-log-endpoint.md:93`), and Option A would not have improved it.

## Outcome

All six stages implemented on `feat/data-plane`. Three things the plan got
wrong, each found by a build or a test rather than by review:

- **Lifting the platform shims put `<winsock2.h>` ahead of libscpi.** Windows
  carries COM's legacy `#define interface struct`, and libscpi uses `interface`
  as a struct member and a parameter name, so every Windows consumer failed to
  compile. The header undefines it.
- **Rule 6's own prescription was too strong.** Requiring C11 atomics would
  have broken the MSVC path; GCC/Clang `__atomic_*` builtins work in C99, so
  `c_std_99` stayed and nothing needed raising.
- **A throttled producer cannot reach the backpressure paths at all.** The
  device's own send buffer is megabytes, so `send()` never blocks and neither
  overflow nor realignment is exercised. Every test that means to reach them
  has to out-run that buffer, and how far depends on kernel auto-tuning.

The last one recurred in three different disguises across Stages 3, 5 and 6. It
is the single most important thing to know before touching this code.

### Verified

| Check | Result |
| --- | --- |
| Linux `ctest` | 6/6, stable across 8 consecutive runs |
| ThreadSanitizer on the ring | clean; races reported with atomics stripped |
| `cargo test` | 90 passed |
| MinGW-w64 cross-build | clean |
| ESP-IDF v5.2.2 | clean |
| Windows 11 guest | 23/23 |
| SocketCAN on `vcan0` | 21/21 end to end |

Each new test is validated by negative control — realignment stubbed out,
atomics stripped, the MTU clamp disabled — so it is known to fail when the
thing it guards regresses.

**Not covered:** real CAN hardware. `vcan0` exercises everything except bus
timing and error frames. MSVC is still unbuilt; the guest has no Visual Studio.
The USB binding (Annex B) is not implemented.

## Risks

| Risk | Handling |
| --- | --- |
| Reconnect misaligns the record stream | `stride` + forward realignment on disconnect; `torn` counter; explicit reconnect-mid-record test |
| Ring ordering on weakly-ordered CPUs | Compiler-builtin acquire/release in Stage 2, before any threaded user; `#error` rather than a volatile fallback |
| Truncated records desyncing the stream | Capacity check before write; never partial-write a record |
| Silent loss anywhere in the chain | 64-bit monotonic totals at glue, producer and source; in-band per record |
| Busy-spin or missed FIN in the drain | Specified poll-based loop with bounded timeout; idle-CPU and idle-disconnect tests |
| New firmware garbling an existing host | Framing off at boot, enabled only by explicit host opt-in |
| A second open port widens exposure | `bind_addr` required with no default, same discipline as the SCPI listener |
| The glue quietly acquiring protocol knowledge | Stage 3's producer is a synthetic generator; CAN does not appear until Stage 5 |

## Non-goals

- **USB488 SRQ.** `usbscpi_tinyusb_set_srq()` stays uncalled and is a deletion
  candidate, not a foundation.
- **Variable-length records on the TCP data plane.** Fixed stride is what makes
  reconnect realignment possible without the glue parsing payloads. The USB
  envelope in Annex B is a separate, framed transport.
- **Bulk transmit / replay** on the data plane. The channel can carry it, but it
  needs host-to-device framing and a pacing policy. Simplex keeps that open.
- **Multiple data consumers.** One attached reader, like the SCPI listener.
- **Replacing `DATA:COUNt?`/`DATA:READ?`.**
- **Interface bring-up / netlink configuration.**
