# SCPI Commands

How a board names its commands, parameters, states and workflows. The goal is
that a user who knows one board can guess the commands of the next.

## Three Patterns

Every command is one of these.

| Pattern | Rule | Examples |
| --- | --- | --- |
| Setting | Write with a value, read the same header with `?`. A read-only value has only the `?` form. | `GPIO 2,1` / `GPIO? 2`, `BLE:CHANnel 37` / `BLE:CHANnel?`, `BLE:SEC?` |
| Action | Does one thing now. No reply; a failure goes to the error queue. | `CAN:SEND 1,#H123,DEADBEEF`, `BLE:DISConnect`, `BLE:PAIR:PASSKey 123456` |
| Job | Anything that runs over time and ends with a bounded result. Always the six commands below. | `BLE:SCAN:*`, `WLAN:SCAN:*`, `BLE:CONNect:*` |
| Stream | A continuous source the user stops: CAN traffic, RSSI samples. `SYSTem:STReam:*`, records on the data plane. | `SYSTem:STReam:STARt` / `:STOP` |

A job `<SUB>` has exactly these commands; leave out only the ones that cannot
apply (a job with no results has no `:COUNt?`, `:FETCh?` or `:CLEar`):

| Command | Meaning |
| --- | --- |
| `<SUB>:STARt [params]` | Clear the previous results and begin. |
| `<SUB>:STOP` | End early. Results so far are kept. |
| `<SUB>:STATe?` | One word: `IDLE`, `RUNNING`, `DONE` or `FAILED`; while waiting on the user, `PASSKEY`, `CONFIRM` or `DISPLAY`. |
| `<SUB>:COUNt?` | Number of results. |
| `<SUB>:FETCh? <index>` | Result `<index>`, 0-based, as comma-separated fields. |
| `<SUB>:CLEar` | Forget the results. |

`FAILED` always comes with an entry in the error queue, queued once per
attempt with `usbscpi_queue_error()` from the `:STATe?` handler (pushing it any
other way fails the query and drops its reply terminator). It is a standard
SCPI error such as `-200,"Execution error"`; device-specific error text is
compiled out, so the stack's own code goes to the device log. There are no
`:STATus?` queries returning raw stack codes.

## Spelling

- Headers use SCPI long form with the short form in capitals: `STARt`,
  `STATe?`, `COUNt?`, `FETCh?`, `CLEar`. Never `START` or `RESult?`.
- A subsystem is a noun (`BLE:SCAN`, `WLAN:SCAN`); the six job words and a
  setting's own name are the only verbs.
- One header per thing. Per-instance headers (`LED:RED`, `LED:GREen`, …) become
  a parameter (`LED red,1`).
- States and enumerated parameter values are words in capitals
  (`KEYBOARD_DISPLAY`), not numbers.

## Parameters

Use these names and types. A parameter that is not here gets a plain lowercase
noun and is added to this table.

| Name | Type | Meaning |
| --- | --- | --- |
| `pin` | u32 | GPIO pin number |
| `value` | bool | Level or on/off of a setting |
| `led` | string | LED name (`red`) or number |
| `channel` | u32 | ADC or radio channel |
| `duration` | u32 | Seconds a job runs |
| `index` | u32 | Row of a job's results, 0-based; declare its options source |
| `key` | u32 | Six-digit passkey |
| `accept` | bool | Accept (1) or reject (0) a numeric comparison |
| `filter` | string | Name substring to match |
| `io` | string | BLE I/O capability word, e.g. `KEYBOARD_DISPLAY` |
| `bus` | u32 | CAN bus number, from 1 |
| `interface` | string | Host network interface, e.g. `can0` |
| `bitrate` | u32 | Bits per second |
| `id` | u32 | CAN identifier; above `0x7FF` is extended |
| `mask` | u32 | CAN filter mask |
| `data` | string | Payload bytes as hex |
| `length` | u32 | Number of bytes |

## Results

Every `:FETCh?` returns the fields its workflow declares with `fields=`, in that
order. A field with the same meaning has the same name everywhere: `addr`
(mac), `rssi` (i32, dBm), `name`, `ssid`, `bssid`, `channel`, `time` (u64, µs),
`id`, `data`.

## Streams

A source with no natural end is a stream, not a job: a job's results must fit
the device's memory and the host must poll for `DONE`, so a quiet source never
finishes and a busy one overflows. A stream runs until the user stops it and
sends every record as it happens.

| Command | Meaning |
| --- | --- |
| `SYSTem:STReam:STARt` | Begin; zero `COUNt?` and `DROPped?`. Refused (error queue) when nothing could arrive, e.g. no CAN bus open. |
| `SYSTem:STReam:STOP` | End. Records already queued are still delivered. |
| `SYSTem:STReam:STATe?` | `<running>,<attached>` (0/1 each). |
| `SYSTem:STReam:COUNt?` / `:DROPped?` | Records produced / lost to a full buffer since `STARt` (u64). |
| `SYSTem:STReam:FORMat?` | `ver=<n>,stride=<bytes>,fields=<schema>`; the same `fields=` grammar as workflows. |
| `SYSTem:STReam:PORT?` | TCP data-plane port. Absent on a USB-only board. |
| `SYSTem:STReam:FRAMing <0/1>` | The host turns on the USB vendor-pipe envelope (`[u8 type][u8 0][u16 len]`, type 2 = one record). |

Every record is `stride` bytes, little-endian, and carries a u64 `dropped`
field: the producer's loss count at capture, so a host reports gaps where they
happened. Name fields as in Results below (`ts_us`, `can_id`, `data`, ...).
A board with more than one vendor interface marks its stream interface with
subclass `0x49`, protocol `0x53`.

## Workflows

- A workflow is named `<subsystem>-<verb>` (`ble-scan`, `wifi-scan`,
  `ble-connect`). The same name means the same behaviour, parameters and result
  fields on every board.
- A job becomes a workflow with `USBSCPI_WF_ACQUIRE` (start, poll, fetch) or
  `USBSCPI_WF_INTERACTIVE` (start, poll, answer prompts) in
  `include/usbscpi/usbscpi.h`; the six job commands are derived from the
  subsystem name.

## Declaring Commands

Declare a board's commands once with `USBSCPI_DEFINE_COMMANDS`. It generates
both the libscpi table and the descriptor table, so every command a host can
call is described. An old header kept for compatibility is an `ALIAS` entry in
the same list: it still works but is not described.

## Renaming

A rename keeps the old name working for one release: an `ALIAS` for a header,
`renamed_from` for a workflow. The host prints the new name when the old one is
used. Remove the old names in the release after.
