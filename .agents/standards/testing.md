# Testing Policy (C)

The parent UI project's Flutter gate lives at `../../.agents/standards/testing.md`.
This one governs everything in `iotsploit-usb`.

## Commit-Time Gate

Before committing C, headers, build configuration, or transport tests, run:

```bash
tools/testing/test-c-full.sh
```

The script configures `build/c-agent-gate` with tests, helpers, and socket glue
enabled, builds it, verifies that the required tests are registered, and runs
CTest, stopping on the first failure. `PYTHON` selects the Python 3
interpreter; Python is required so transport coverage cannot silently vanish.

## What The Gate Includes

- `test_usbscpi` — core unit tests.
- `test_glue_tinyusb` — TinyUSB glue against `tests/stub/tusb.h`.
- `test_stm32f4_socketcan_identity` — actual board identity/descriptor code with
  a fake 96-bit UID; verifies programmer byte order and the full SCPI version.
- `test_scpi_tcp_smoke` — the daemon over real sockets.
- `test_stream_dataplane` — the stream glue driven by `stream_testgen`.
- Linux only: `test_ring_concurrent` and `test_can_capture`.

## Skips

`test_can_capture` needs a `vcan0` interface and can-utils. Without them it
prints `SKIP:` and exits 0, so CTest reports it as passed. The gate lists every
`SKIP` line after the run. A skipped test is not validation; report it.

## What The Gate Excludes

- Firmware builds. For a board-specific change, build the affected example with
  its own toolchain (see [`c.md`](c.md)) and report the result.
- `host/rust`. Run `cargo test` there when a change touches it or a wire
  contract.
- Hardware. Host tests and firmware builds do not prove device behavior. Say
  whether a change was flashed and exercised on hardware, and on which board.

## Agent Responsibilities

1. **Stop and do not commit** when the runner exits non-zero.
2. **Fix failures caused by your changes** and rerun the complete gate.
3. **Report counts**: passed, failed, and skipped, plus firmware and hardware
   validation status.
4. **Report environmental blockers** (no Python, no vcan, no toolchain, no
   board) rather than silently skipping required checks.
5. **Never delete, weaken, or skip an unrelated test** only to make the gate
   pass.
6. **Never use `git commit --no-verify`** without explicit user authorization.

## Writing Tests

Use existing coverage and focused validation first. Add a minimal test only for
an uncovered, high-risk regression path, and register it with CTest in
`CMakeLists.txt` so the gate runs it. Transport behavior belongs in the Python
tests against the daemon or `stream_testgen`; core logic belongs in
`tests/test_usbscpi.c`. Never reach real hardware from a CTest test unless it
skips cleanly without it.

## Git Enforcement

`iotsploit-usb` is its own git repository. Enable the hook once per working
copy, from this directory:

```bash
git config core.hooksPath tools/git-hooks
```

The tracked `tools/git-hooks/pre-commit` hook runs the full gate.
