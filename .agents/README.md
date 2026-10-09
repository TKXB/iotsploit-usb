# C Agent Instructions

Canonical index of the standards for `iotsploit-usb`.

| Standard | Covers |
| --- | --- |
| [`standards/testing.md`](standards/testing.md) | Commit-time gate, skips, firmware and hardware validation, git enforcement |
| [`standards/c.md`](standards/c.md) | Toolchain, language level, warnings, buffers, callbacks, vendored code |
| [`standards/firmware-design.md`](standards/firmware-design.md) | Core independence, board and example ownership |
| [`standards/integration.md`](standards/integration.md) | Core, transport, board, and host boundaries |
| [`standards/scpi-commands.md`](standards/scpi-commands.md) | Command patterns, job states, parameter names, workflows |

The single command to run before committing C, headers, build configuration,
or transport tests:

```bash
tools/testing/test-c-full.sh
```
