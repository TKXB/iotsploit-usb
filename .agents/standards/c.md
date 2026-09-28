# C

## Toolchain

`CMakeLists.txt` is the configuration of record. The core, helpers, and glue
build as C99 (`c_std_99`); MSVC builds as C11 (`c_std_11`) because it has no
C99 mode and the daemon needs designated initializers.

Common host commands, from this directory:

```bash
cmake -S . -B build -DUSBSCPI_BUILD_SOCKET_GLUE=ON
cmake --build build
ctest --test-dir build --output-on-failure
tools/testing/test-c-full.sh         # the commit-time gate (build/c-agent-gate)
```

Firmware examples use their own build systems; see each example's README:
`idf.py build` (`examples/esp32s3`, `examples/esp32s3-spp-security`), `make`
(`examples/stm32f4disco`, `examples/nrf52840`, `examples/butterfly-nrf52840`),
and the Pico SDK via `cmake/iotsploit-usb-pico.cmake` (`examples/pico2`).

## Warnings

`USBSCPI_WARNINGS` is `-Wall -Wextra -Werror` (`/W4` on MSVC). It applies to
`usbscpi`, the socket and stream glue, the daemon, the CAN example, and
`stream_testgen`. Preserve these settings: do not lower the level, add blanket
`-Wno-*` flags, or remove a target from `USBSCPI_WARNINGS` to make code
compile. Fix the warning.

## Vendored Code

`third_party/libscpi` is vendored. It builds as its own static library so its
warnings do not trip `-Werror`. Do not edit it; configure it through
`third_party/libscpi/inc/scpi/scpi_user_config.h` (`SCPI_USER_CONFIG=1`).
Build artifacts and fetched components (`managed_components/`, `_build/`,
`build/`, `host/rust/target/`) are not sources.

## Memory And Buffers

- No dynamic allocation after init. `usbscpi_init()` takes caller-owned
  `storage` sized by `usbscpi_sizeof()`, plus caller-owned `line_buf` and
  `io_buf`. The caller keeps all of them, the `usbscpi_config_t` pointers, and
  the descriptor tables alive for the context's lifetime.
- Every write is bounded by an explicit length. Report overflow through the
  existing `usbscpi_status_t` codes (`USBSCPI_ERR_OVERFLOW`, ...) rather than
  truncating silently.
- Binary blocks are length-delimited; never assume payloads are
  NUL-terminated or free of `\n`, `\r`, `0x00`, or `#`.

## Callbacks

Callbacks receive the config's `user` pointer. Data passed to a callback is
valid only for that call; copy what must outlive it. `lock`/`unlock` guard
state shared with another context (ISR, RTOS task, thread); keep critical
sections short and never call back into the core while holding them.

## Formatting

There is no formatter. Match the surrounding file. Do not reformat existing
sources as part of an unrelated change.
