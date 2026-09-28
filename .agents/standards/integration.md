# Integration

## Boundaries

```text
host (host/rust, Python tests)
   │  SCPI over USBTMC, TCP 5025, or the USB vendor pipe
glue (tinyusb | socket | stream)
   │  usbscpi_config_t callbacks, usbscpi_on_rx(), usbscpi_task()
core (src/usbscpi.c + libscpi)
   │  scpi_command_t handlers registered by the example
example / board (examples/*)
```

- The core exposes only `include/usbscpi/usbscpi.h`. Transports call
  `usbscpi_on_rx()` and supply `usb_tx`; the core never calls a stack directly.
- One `usbscpi_t` context per transport. `usbscpi_config_t` carries exactly
  one `usb_tx`; serving two transports means two contexts.
- The daemon (`examples/daemon`) is the device side as a desktop program and
  the hardware-free rig for the host and the Python transport tests.

## Wire Contracts

Treat the SCPI command set, descriptor grammar, block format
(`#<N><LEN><payload>`), and stream record format as contracts shared with
`host/rust`. Change both sides together, and cover the change with the
existing transport tests.

## Host Consumers

`host/rust` is a separate Cargo project; it is not built by the C gate. When a
change touches it or a contract it parses, run from `host/rust`:

```bash
cargo test
```

Python transport tests live in `tests/` and run through CTest; see
[`testing.md`](testing.md).
