# Firmware Design

## Core Independence

`src/usbscpi.c` and `include/usbscpi/` know SCPI framing, blocks,
descriptors, and workflows. They do not include USB stack, RTOS, socket, or
board support headers. A change that needs one of those belongs in glue or in
an example.

## Owners

| Concern | Owner |
| --- | --- |
| SCPI parsing, block transfer, descriptor emission | `src/usbscpi.c` |
| Lock-free SPSC ring | `helpers/ring_buffer.c` |
| USBTMC over TinyUSB | `glue/usbscpi_tinyusb.c` |
| SCPI over BSD sockets (glibc, lwIP, Winsock) | `glue/usbscpi_socket.c` |
| Payload-agnostic record stream | `glue/usbscpi_stream.c` |
| Board init, clocks, pins, radio, USB descriptors | `examples/<board>/` |
| Protocol-specific producers (for example CAN) | `examples/<consumer>/` |

Fix behavior at its owner. Board-specific quirks stay in that board's example;
protocol knowledge never enters the stream glue (`tests/stream_testgen.c`
proves it stays payload-agnostic).

## Reuse Before Adding

Before adding a command table, ring, transport path, or descriptor helper,
search the core, helpers, glue, and the other examples. Extend the existing
owner; do not copy it into a board.

## Descriptors

A command a host must discover goes in the descriptor as well as the SCPI
command table; hosts discover commands and workflows through
`SYSTem:HELP:DESCription?`.
