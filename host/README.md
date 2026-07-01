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
querying (`SYSTem:HELP:DESCription?`). The default (Linux kernel) build is
zero-dependency; the `raw-usb` feature pulls in `nusb` (pure-Rust, no libusb)
plus `futures-lite`/`async-io` for cross-platform Windows/macOS/Linux support.

See [rust/README.md](rust/README.md) for full usage.


