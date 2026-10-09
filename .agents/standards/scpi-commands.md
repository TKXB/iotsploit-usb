# SCPI Commands

How a board names its commands, parameters, states and workflows. The goal is
that a user who knows one board can guess the commands of the next.

## Three Patterns

Every command is one of these.

| Pattern | Rule | Examples |
| --- | --- | --- |
| Setting | Write with a value, read the same header with `?`. A read-only value has only the `?` form. | `GPIO 2,1` / `GPIO? 2`, `BLE:CHANnel 37` / `BLE:CHANnel?`, `BLE:SEC?` |
| Action | Does one thing now. No reply; a failure goes to the error queue. | `CAN:SEND 1,#H123,DEADBEEF`, `BLE:DISConnect`, `BLE:PAIR:PASSKey 123456` |
| Job | Anything that runs over time. Always the six commands below. | `BLE:SCAN:*`, `WLAN:SCAN:*`, `CAN:CAPTure:*`, `BLE:CONNect:*` |

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
- A subsystem is a noun (`BLE:SCAN`, `CAN:CAPTure`); the six job words and a
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

## Workflows

- A workflow is named `<subsystem>-<verb>` (`ble-scan`, `wifi-scan`,
  `can-capture`). The same name means the same behaviour, parameters and result
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
