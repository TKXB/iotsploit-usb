# ESP32-S3 HID integration — implementation and validation status

Issue: TKX-209. Updated: 2026-10-08.
Base implementation: `46936c7a552368fb529401cc5ff5fe7ae5f88ddc`.
Lifecycle fixes and validation are recorded for `tkx-209-esp32s3-evilduck`.

## Actual firmware profile

`examples/esp32s3-hid/` builds **HID-only USB + Wi-Fi TCP control**.
USB identity is `1209:0003`, product `IoTSploit ESP32-S3 HID`, one keyboard
interface. USBTMC and vendor classes/glue are disabled. SCPI uses TCP port 5025;
Wi-Fi STA credentials are supplied through the existing NET_SCPI build defines.
The raw listener remains unauthenticated and is intended for a dedicated test
network. There is no composite build profile in this candidate.

The earlier composite `1209:0002` / `/dev/usbtmc3` / no-Wi-Fi measurements belong
to a different prototype. They are not evidence for this firmware. Persistence,
MSC, additional locales and language expansion remain deferred.

## Lifecycle corrections

- RUN initialises its immutable script snapshot and publishes RUNNING under the
  runner mutex before another SCPI command can query or cancel it. HID reports
  are produced only by the USB task. STOP also takes that mutex.
- Socket glue offers optional connect/disconnect notifications, leaving the
  existing serve API intact. Each HID control connection receives a new owner
  token. Disconnect discards that session's staged upload and cancels its run
  before another session is served.
- The final neutral HID report must complete before DONE. Submission failure
  produces ERROR. Busy or incomplete release has a 100 ms finish deadline, so
  STOP/runtime expiry cannot leave the runner permanently RUNNING.
- Wi-Fi association retries remain eligible to start the control listener after
  the former 30-second startup cutoff.
- Runner diagnostic/state snapshots are protected across the USB/socket tasks.
  The parameter descriptor uses a complete designated initializer.

## Candidate and evidence

Build: ESP-IDF v5.2.2, app version `0.1.0`, app image size `0xc5b80`
(47% partition space free). Application image SHA-256:

```text
1162af3ad2716b304ac007b7d858c91d9d495738b07d0a7a0a123b0645ef5356
```

Hardware identity: board MAC `34:85:18:41:C6:AC`, native USB bus path `1-4.3`,
UART/programmer `/dev/ttyUSB2` (CP2102N serial
`4c4818609ae7ec11bcd67e60e89bdf6f`). Its STA control address on the test network is
`10.42.0.125:5025`; this address is rig-specific, not a firmware contract.

Validation of the lifecycle candidate:

- Native C gate: **8 passed, 0 failed, 0 skips**, including registered portable
  release-completion/deadline tests and real-socket session cleanup regressions.
- Rust: **73 passed, 0 failed**.
- Firmware: HID example builds without the prior initializer warning.
- Linux hardware: captured `aB!` with matching modifier/key releases while the
  keyboard was exclusively grabbed; 40 immediate RUN/STOP cycles cancelled;
  TCP disconnect cancelled execution; truncated upload cleanup released the
  reservation; a fresh session without START was rejected; protocol recovery
  retained the same boot token.
- Hardware results: `/tmp/tkx209-hardware-results.json`; flash transcript:
  `/tmp/tkx209-final-flash.log`; additional validation results are reported on
  TKX-209. These are local evidence paths, not checked-in test artifacts.
- Late Wi-Fi hardware: the listener recovered when the hotspot returned at least
  35 seconds after boot (`/tmp/tkx209-late-wifi-results.json`).
- Windows enumeration/typing and USB suspend are not yet validated on hardware.

## Required firmware fuzz gate: passed

The Python gate now selects a native SCPI TCP harness for `esp32s3-hid` and
retains the existing USBTMC path for other targets. The campaign flashed the exact
images in the candidate manifest through the Rust flasher on `/dev/ttyUSB2`,
verified product/version/board serial, and monitored boot continuity throughout.

- Target: `esp32s3-hid`, HID-only + TCP, `10.42.0.125:5025`.
- Application SHA-256: `1162af3ad2716b304ac007b7d858c91d9d495738b07d0a7a0a123b0645ef5356`.
- Seed: 47; requested iterations: 64 per strategy/case.
- Executed: **544 mutations + 4 valid seeds + 4 retained regressions = 552 cases**.
- Failed/crashed cases: **0**; all four mutation phases completed.
- Retained cases: immediate RUN/STOP, disconnect cancellation, truncated upload
  ownership cleanup, and malformed-command reconnect recovery.
- Baseline/final boot token: `b91a156bd315307c` / `b91a156bd315307c`.
- Evidence directory: `/tmp/tkx209-gate/tcp-acceptance/esp32s3-hid/`
  (`result.json`, `flash.log`, campaign JSONL, exact payload blobs).
- Inputs: `/tmp/tkx209-gate/manifest.json`, `/tmp/tkx209-gate/rig.json`, and
  Python `conf/fuzz/regressions/esp32s3-hid.jsonl`.
- Python commit gate: **1,957 passed, 5 skipped, 45 warnings**; Ruff, import smoke,
  and portability checks passed. TCP harness tests cover framing, deadlines,
  limits, cancellation, identity assertions and reconnect cleanup.

The result records pre-commit source/content fingerprints and all flashed image
hashes; the TKX-209 reply attaches the campaign evidence. These measurements are
for this HID-only image, rather than the earlier composite prototype.
