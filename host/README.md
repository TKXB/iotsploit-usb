# host/

Upper-computer (PC-side) implementations for controlling `iotsploit-usb`
devices over SCPI/USBTMC. Each subdirectory is a self-contained host in a
different language.

## rust/

A generic, cross-platform CLI host (`iotsploit-host`). It auto-detects any
`iotsploit-usb` device, discovers its SCPI command set via
`SYSTem:HELP:HEADers?`, and supports text queries, binary-block reads,
capabilities parsing, error-queue draining, an interactive REPL,
line-record profiles, a generic workflow engine, and on-device descriptor
querying (`SYSTem:HELP:DESCription?`). The core crate is zero-dependency;
the only optional dependency is `rusb` behind the `raw-usb` feature for
Windows/macOS.

See [rust/README.md](rust/README.md) for full usage.

## python/

The original Python smoke-test host (`scan_test.py`) for the ESP32-S3 demo.
It drives Wi-Fi scan, BLE scan, and BLE connect/pair workflows through the
Linux `/dev/usbtmcN` kernel driver. Kept as a reference; the Rust host now
has full workflow parity via `iotsploit-host workflow <name>` (metadata read
live from the device descriptor).
