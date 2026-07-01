# iotsploit-usb

`iotsploit-usb` is a small C99 component that lets firmware projects add a USBTMC + SCPI control interface without binding the core to a USB stack, RTOS, DMA driver, or board support package.

The public C API uses the `usbscpi_*` prefix because it describes the component interface and avoids churn for downstream firmware code.

The host project owns USB initialization and device-specific I/O. This component owns the SCPI command path, definite-length binary block parsing, standard error queue, and response routing.

## Design Choices

- Stack-neutral core: no TinyUSB, ESP-IDF, Pico SDK, STM32, or RTOS headers in `src/`.
- Opaque handle: host allocates storage with `usbscpi_sizeof()` and calls `usbscpi_init()`.
- No dynamic allocation after init: all buffers are supplied by the host.
- libscpi-facing API: host commands are registered as `scpi_command_t` tables and callbacks use `SCPI_Result*`.
- Binary blocks bypass the text parser: `:DATA:WRITE #<n><len><payload>` payload bytes go directly to callbacks.
- Optional helpers only: ring buffer and TinyUSB glue are separate from the core library.

## Repository Layout

```text
include/usbscpi/usbscpi.h       public component API
include/usbscpi/ring_buffer.h   optional SPSC ring helper
src/usbscpi.c                   core receive path and standard commands
third_party/libscpi/            vendored libscpi v2.3
helpers/ring_buffer.c           optional ring helper
glue/usbscpi_tinyusb.c          optional TinyUSB USBTMC adapter
examples/pico2/                 Raspberry Pi Pico (RP2040) TinyUSB demo
examples/esp32s3/               ESP32-S3 (ESP-IDF + TinyUSB) demo, hardware-verified
host/rust/                     generic Rust CLI host (cross-platform)
tests/test_usbscpi.c            host unit tests
```

## Minimal Integration

```c
static uint8_t usbscpi_storage[1024];
static char line_buf[96];

static int usb_tx(void *user, const uint8_t *data, size_t len, bool eom) {
    return platform_usbtmc_send(data, len, eom);
}

static int on_block_data(void *user, const uint8_t *data, size_t len) {
    return platform_write_to_dma_or_ring(data, len);
}

usbscpi_config_t cfg = {
    .usb_tx = usb_tx,
    .on_block_data = on_block_data,
    .line_buf = line_buf,
    .line_buf_len = sizeof(line_buf),
    .max_block_len = 4096,
    .idn = "Vendor,Product,SN0001,0.1.0",
};

usbscpi_t *scpi = usbscpi_init(usbscpi_storage, sizeof(usbscpi_storage), &cfg);
```

When the USB stack receives a device-dependent message, pass its payload to:

```c
usbscpi_on_rx(scpi, data, len, eom);
```

## Default Commands

- `*IDN?`
- `*RST`
- `*CLS`
- `*OPC?`
- `SYSTem:ERRor?`
- `DATA:FREE?`
- `SYSTem:HELP:HEADers?` — list all registered command headers
- `SYSTem:HELP:DESCription?` — emit a **line-record descriptor** (optional;
  set `cfg.descriptor` to enable). Returns an IEEE 488.2 block containing
  `DEV`, `CMD`, and `WF` records that the Rust host parses to discover
  commands, parameters, and workflows. See
  [`include/usbscpi/usbscpi.h`](include/usbscpi/usbscpi.h) for the
  `usbscpi_descriptor_t` struct.

Register project-specific commands with:

```c
static scpi_result_t meas_voltage(scpi_t *ctx) {
    return SCPI_ResultUInt32(ctx, 3300);
}

static const scpi_command_t app_cmds[] = {
    { "MEASure:VOLTage?", meas_voltage, 0 },
    SCPI_CMD_LIST_END
};

usbscpi_register(scpi, app_cmds);
```

## Binary Data Contract

The MVP wire format is intentionally frozen:

```text
:DATA:WRITE #<N><LEN><LEN bytes payload>
```

`N` is one ASCII digit from `1` to `9`, and `LEN` is an ASCII decimal field of `N` digits. Payload completion is length-driven and does not depend on `\n`, so payload may contain `0x00`, `\n`, `\r`, `#`, or any other byte.

The `:DATA:WRITE ` prefix is intentionally fixed so the component can switch from text mode to binary mode before the payload reaches the SCPI parser.

## Build

Host CMake:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

ESP-IDF:

```cmake
set(EXTRA_COMPONENT_DIRS path/to/iotsploit-usb)
```

Pico SDK:

```cmake
include(path/to/iotsploit-usb/cmake/iotsploit-usb-pico.cmake)
target_link_libraries(your_firmware PRIVATE usbscpi)
```

## TinyUSB Glue

The core does not depend on TinyUSB. Projects that use TinyUSB can either wire callbacks directly or build `glue/usbscpi_tinyusb.c` and configure:

```c
cfg.usb_tx = usbscpi_tinyusb_tx;
usbscpi_tinyusb_bind(scpi);
```

The glue owns the **entire** USBTMC bulk-IN response path. USBTMC is a two-phase
protocol: the device receives the command on bulk-OUT, and may only send the
reply *after* the host issues a bulk-IN read (TinyUSB enforces this with
`TU_VERIFY(state == STATE_TX_REQUESTED)` inside `tud_usbtmc_transmit_dev_msg_data`).
The component produces the reply synchronously in the OUT path, so the glue
**buffers** it (`usbscpi_tinyusb_tx`, raising the MAV status bit) and
**transmits** it later from `tud_usbtmc_msgBulkIn_request_cb`. Sending the reply
directly from the OUT path silently drops it and makes every query read time out
(`-110`).

The glue also owns the **bulk-OUT arming**. It defines `tud_usbtmc_open_cb` to
call `tud_usbtmc_start_bus_read()` when the interface is opened, and re-arms the
OUT endpoint after every receive, IN-completion and clear. TinyUSB opens the
endpoint but leaves the *first* bus read to the application; if nothing arms it,
enumeration still succeeds but every host write times out (`-110`). Letting the
glue own this means consumers cannot forget it.

Because of this, integrators using the glue **must not** also define any of:
`tud_usbtmc_open_cb`, `tud_usbtmc_msg_data_cb`,
`tud_usbtmc_bulkOut_clearFeature_cb`, `tud_usbtmc_msgBulkIn_request_cb`,
`tud_usbtmc_msgBulkIn_complete_cb`, `tud_usbtmc_bulkIn_clearFeature_cb`,
`tud_usbtmc_get_stb_cb` (the glue provides them; duplicates break linking).
Route any out-of-band device→host bytes (e.g. UDS replies) through
`usbscpi_tinyusb_queue_response()` rather than calling
`tud_usbtmc_transmit_dev_msg_data()` directly.

The glue does **not** cover the remaining strong-symbol USBTMC callbacks that
TinyUSB's `usbtmc_device.c` requires (they have no library default). Every
application must still define them or the link fails — see `examples/pico2/main.c`:
`tud_usbtmc_get_capabilities_cb`, `tud_usbtmc_msgBulkOut_start_cb`,
`tud_usbtmc_initiate_abort_bulk_out_cb`, `tud_usbtmc_check_abort_bulk_out_cb`,
`tud_usbtmc_initiate_abort_bulk_in_cb`, `tud_usbtmc_check_abort_bulk_in_cb`,
`tud_usbtmc_initiate_clear_cb`, `tud_usbtmc_check_clear_cb`.

## Extension Points

These interfaces are deliberately present in the MVP so later features can be added without changing the core contract:

- `usb_tx`: swap USB stack glue.
- `on_block_begin/data/end`: add CRC, sequence numbers, or zero-copy forwarding.
- `data_free`: implement `DATA:FREE?` with a ring buffer or downstream queue.
- `lock`/`unlock`: protect callbacks when called from an ISR or USB task.
- `usbscpi_clear`: map USBTMC clear/abort requests and SCPI reset paths to one state reset.
- `descriptor`: optional `usbscpi_descriptor_t` that enables `SYSTem:HELP:DESCription?`
  to emit command/workflow metadata as line-record text. The Rust host uses
  this for typed help, parameter validation, and workflow automation.

## Scope

This repository is a reusable component, not a complete firmware application. It does not initialize clocks, USB descriptors, RTOS tasks, DMA, SPI, UART, bootloaders, or board pins.
