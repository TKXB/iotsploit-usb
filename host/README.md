# host/

Upper-computer (PC-side) implementations for controlling `iotsploit-usb`
devices over SCPI/USBTMC. Each subdirectory is a self-contained host in a
different language.

## rust/

A generic, cross-platform CLI host (`iotsploit-host`) for USB (USBTMC) and
network (raw SCPI on TCP 5025) devices. `devices` lists connected boards,
`info` and `help` show what a board is and what it can do (from the board's own
`SYSTem:HELP:DESCription?`), `send` runs any SCPI command and reports device
errors, and `workflow` runs the multi-step jobs the board describes. The
default build (Linux kernel USBTMC + TCP) has no dependencies; the `raw-usb`
feature adds `nusb` (pure Rust, no libusb) for Windows and macOS.

See [rust/README.md](rust/README.md) for full usage.


