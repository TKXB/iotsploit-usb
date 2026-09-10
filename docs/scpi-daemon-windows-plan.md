# SCPI-over-TCP server on Linux and Windows

## Goal

Make the device-side SCPI server — the thing a host connects *to* — a first-class
desktop binary on both Linux and Windows. Same command surface, same descriptor
discovery, same binary block contract, same `tcp:host:5025` device string on the
host side.

## Scope: TCP only

The desktop server speaks raw SCPI on TCP 5025 and nothing else. No USB device
mode on either platform — the USB path stays where it belongs, on ESP32-S3,
Pico, nRF52840 and STM32, and this plan does not touch `glue/usbscpi_tinyusb.c`.

Linux already worked before this plan (`examples/linux/`, built with
`-DUSBSCPI_BUILD_SOCKET_GLUE=ON`). Windows is the new half; the rest of the work
is making the example stop calling itself Linux-only.

This is a *glue-tier and build-tier* change. `include/usbscpi/usbscpi.h`,
`src/usbscpi.c`, `helpers/` and `third_party/libscpi/` are already free of
platform headers — `src/usbscpi.c` includes only `ctype/stdarg/stdio/string`
plus `scpi/scpi.h` — and must stay untouched.

## What actually blocks Windows

Everything is confined to `glue/usbscpi_socket.c` and `examples/linux/main.c`.

### `glue/usbscpi_socket.c`

1. **POSIX headers** (`arpa/inet.h`, `netinet/in.h`, `netinet/tcp.h`,
   `sys/socket.h`, `unistd.h`, lines 10-15) → `winsock2.h` + `ws2tcpip.h`.
2. **No `WSAStartup`.** Winsock refuses every call until the library is
   initialised. Do it lazily inside `usbscpi_socket_serve()` rather than adding
   a public `usbscpi_socket_init()` — the API then stays identical on all three
   platforms (glibc, lwIP, Winsock) and callers cannot forget it.
3. **Socket handle type.** `int fd` / `fd < 0` (lines 28, 38, 59, 95) is wrong on
   Win64: `SOCKET` is `UINT_PTR` and the sentinel is `INVALID_SOCKET`, not `-1`.
   Introduce one file-local `usbscpi_sock_t` typedef and an `INVALID` constant.
4. **`close()` → `closesocket()`** (lines 87, 108, 113, 119, 134).
5. **`errno`/`EINTR`** (lines 50, 77, 126). Winsock reports through
   `WSAGetLastError()`, and blocking calls do not return `WSAEINTR` in modern
   usage. The retry branches collapse to nothing on Windows.
6. **`send`/`recv` take `int` length**, not `size_t`. `len - sent` (line 45)
   needs a clamped cast, or a >2 GB block write is undefined.
7. **`SO_REUSEADDR` means something different and worse on Windows** (line 101):
   it lets an unrelated process bind the *same* live port and steal connections.
   Windows should get `SO_EXCLUSIVEADDRUSE` instead — the closest thing to the
   POSIX intent, and a real security difference for a listener that exposes the
   whole SCPI command surface.
8. `MSG_NOSIGNAL` (line 19) is already guarded to `0` and there is no SIGPIPE on
   Windows. No change. `setsockopt` optval is already cast to `const char *`.
   Someone anticipated this; keep it.

### `examples/linux/main.c`

9. **`signal(SIGPIPE, SIG_IGN)`** — `SIGPIPE` does not exist on Windows. Guard
   it; it is a no-op there because Winsock has no such signal.
10. **Rename `examples/linux/` → `examples/daemon/`** and the target
    `usbscpi_linux` → `usbscpi_daemon`. The directory name is now a lie: the same
    `main.c` is the Pi 5 daemon, the Windows daemon, and the hardware-free test
    rig for the Rust host. Nothing in it is Linux-specific.

### `CMakeLists.txt`

11. Link `ws2_32` on Windows.
12. `target_compile_features(... c_std_99)` is a no-op for MSVC. `main.c` uses
    designated initializers in `desc_workflows`, so MSVC needs `/std:c11`
    (VS2019 16.8+). Request `c_std_11` on MSVC only; leave C99 everywhere else so
    the embedded toolchains are unaffected.
13. Give the glue and daemon targets the same `if(MSVC) /W4 else -Wall -Wextra
    -Werror` treatment `usbscpi` already has, plus `_CRT_SECURE_NO_WARNINGS`.

## Where the `#ifdef`s live

One platform block at the top of `glue/usbscpi_socket.c`, next to the existing
`ESP_PLATFORM` branch — three tiny shims (`usbscpi_sock_t`, a close wrapper, a
"was this EINTR" predicate) and the header selection. Not a new file, not a
`platform/` directory, not an abstraction layer. The glue already *is* the
platform owner for sockets; a second layer would be a parallel abstraction with
one caller.

The file's header comment already says "Builds against glibc on Linux and
against lwIP on ESP-IDF" — this extends that sentence, it does not change the
design.

## Staging

| Stage | Change | Verified by |
| --- | --- | --- |
| 1 | Platform shims + Winsock port in `usbscpi_socket.c` | Linux build still clean; cross-build links |
| 2 | Rename `examples/linux` → `examples/daemon`, guard `SIGPIPE` | both platforms build |
| 3 | CMake: `ws2_32`, MSVC standard/warnings | cross-build + native Linux |
| 4 | README: replace the Linux-only build block with a Linux/Windows one | — |

Stage 1 is the only one with real risk. Stages 2-4 are mechanical.

## Verification

MinGW-w64 is already installed on this machine (`x86_64-w64-mingw32-gcc`), so
the Windows build is verifiable here without a Windows box:

```sh
# Linux — must stay green
cmake -S . -B build -DUSBSCPI_BUILD_SOCKET_GLUE=ON
cmake --build build && ctest --test-dir build --output-on-failure

# Windows cross-compile — compile + link only
cmake -S . -B build-win -DUSBSCPI_BUILD_SOCKET_GLUE=ON \
      -DUSBSCPI_BUILD_TESTS=OFF \
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
      -DCMAKE_SYSTEM_NAME=Windows
cmake --build build-win
```

There is no Wine here, so the cross-build proves it compiles and links, not that
it serves. Runtime confirmation needs one manual pass on a real Windows machine:
start `usbscpi_daemon.exe 127.0.0.1 5025`, then from the Rust host run
descriptor discovery, the `demo-scan` workflow, and `DEMO:DATA? 4096` (the
definite-length block path — the one most likely to expose a `send()` length or
partial-write bug). MSVC likewise cannot be checked from here; item 12 is the
one most likely to bite there.

## Outcome

Implemented on `feat/tcp-server-windows`. All four stages landed as written,
with two deviations found during implementation:

- **`_WIN32_WINNT` must precede every include, not just the socket headers.**
  The plan assumed a guard next to `<winsock2.h>` would do. It does not: MinGW's
  CRT headers latch `_WIN32_WINNT` to a pre-Vista default the first time any of
  them is pulled in — including via `<stdint.h>` from our own public header — so
  `inet_pton()` came out undeclared. The define now sits above the first
  `#include` and only ever raises the value, never lowers one the consumer's
  build system chose.
- **Item 13 (`-Werror` on the daemon) surfaced pre-existing partial
  initializers** in the example's descriptor tables, which omit the optional
  trailing `options_*_query` fields. Those tables now use designated
  initializers, which is both what the omission meant and what silences
  `-Wmissing-field-initializers` properly.

### Verified

| Check | Result |
| --- | --- |
| Linux build, `-Wall -Wextra -Werror` | clean |
| Linux `ctest` | 2/2 pass |
| Windows cross-build (MinGW-w64, `-static`) | clean, no warnings from our sources |
| ESP-IDF v5.2.2 / lwIP build of `examples/esp32s3` | clean — third stack unaffected |
| Functional suite, Linux daemon | 23/23 pass |
| Functional suite, Windows 11 guest over SSH tunnel | 23/23 pass |
| Second bind on a live port (Windows) | refused — `SO_EXCLUSIVEADDRUSE` confirmed |

The functional suite covers `*IDN?`, non-query commands sending nothing,
batched commands after a failure, the error queue, the `demo-scan`
trigger_poll_fetch workflow, definite-length block reads at 16/512/4096 B,
`SYSTem:HELP:HEADers?` blank-line framing, descriptor discovery, and
reconnect-after-dirty-disconnect.

**Still unverified: MSVC.** The Windows guest has no Visual Studio, so item 12
(`c_std_11` for designated initializers) is reasoned-about, not tested. The
MinGW path is proven end to end.

## Non-goals

- Anything on the USB transport, on any platform.
- Multi-client serving. Backlog stays 1: one `usbscpi_t` has one line buffer and
  one error queue, and that reasoning is unchanged by the platform.
- Windows CI. Worth doing once the port lands, but it is a separate change and
  this repo has no CI to extend yet.
- Anything on the Rust host. Its TCP backend is already portable; only its
  Linux-only USBTMC kernel backend is not, and that is out of scope here.
