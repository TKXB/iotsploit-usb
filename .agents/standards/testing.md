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

## Built-In Firmware Fuzzing

Agents must run this additional gate before committing a firmware feature change,
as required by `AGENTS.md`. This policy is stricter than the default human Git
hook: the C test script remains hardware-free, and the agent invokes fuzzing
separately. Documentation-only commits are exempt.

Use a Python checkout containing `tools/hardware/firmware_fuzz_gate.py`, its
tracked `conf/fuzz/targets.json` and `conf/fuzz/features/*.json`, and the project's
Poetry environment. The headless application needs the `iotsploit-fuzzer`,
`iotsploit-django` and `iotsploit-core` packages and USB dependencies; it needs
no running Django server or Redis. Keep machine-specific rig bindings outside
tracked feature suites.

Build the required board examples from this exact candidate tree and build the
existing UI `firmware-flasher` CLI. Generate a candidate manifest in the existing
flash-manifest format, with absolute image paths, versions and SHA-256 hashes.
Each required board must expose the boot telemetry expected by the profile.

From the Python checkout, with the paths set to the actual candidate and rig:

```bash
poetry run python tools/hardware/firmware_fuzz_gate.py \
  --rig /path/to/rigs.json --manifest /path/to/candidate-manifest.json \
  --ui-root /path/to/ui --firmware-root /path/to/iotsploit-usb \
  --flasher /path/to/ui/firmware/target/debug/firmware-flasher \
  --flash --iterations 64 --seed 47 --output artifacts/firmware-fuzz
```

The command runs the entire configured acceptance matrix. For a board-specific
change, use `--target <profile-name>` for each affected target. For shared
core/USBTMC changes, run all required targets. A feature shared by several boards
requires every affected board/variant. Add the new feature's valid workflow,
parameter boundaries and malformed cases to its suite before running the gate;
an unrelated passing baseline is insufficient.

When retained regressions exist, pass `--replay /path/to/campaign.jsonl`; the
runner replays their complete ordered prefix before fresh mutation. Preserve
exact inputs, transcripts and observations with the result artifacts. Source
revisions/content fingerprints and flashed image hashes must describe the code
being committed; repeat validation if firmware behavior changes afterward.

Exit `0` means every required target passed. Exit `1` is a campaign failure and
exit `2` is incomplete setup/validation. Both block an agent's feature commit.
Unavailable hardware or a missing runner is a blocker, not permission to skip.
Report it and leave the feature uncommitted. The current observer detects boot
changes and protocol recovery failures; it does not establish absence of resource
leaks or replace independent UART/probe monitoring.

## Git Enforcement

`iotsploit-usb` is its own git repository. Enable the hook once per working
copy, from this directory:

```bash
git config core.hooksPath tools/git-hooks
```

The tracked `tools/git-hooks/pre-commit` hook runs the full gate.
