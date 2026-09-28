# C Agent Instructions

This file is authoritative for `iotsploit-usb`. Read the standards indexed in
[.agents/README.md](.agents/README.md) before changing the relevant owner.

## Core Principles

Less is more. Prefer Delete > Replace > Add.

- Solve behavior at its owner. Fix triggers rather than hiding broken logic
  behind initialization flags, skip-first-call branches, or exception guards.
- Search the repository first. Reuse the core, helpers, and transport glue;
  extend existing owners rather than adding parallel abstractions.
- Prefer deleting or modifying existing code. Justify new files; implement
  the simplest complete solution without speculative layers or unrelated cleanup.
- Understand what you remove, remove code made unused by your change, and
  validate existing behavior. Do not retain broken paths as insurance.
- Use existing coverage and focused validation. Add minimal tests only for
  uncovered, high-risk regression paths; read the testing standard first.

## C Test Gate

Before committing C, headers, build configuration, or transport test changes:

```bash
tools/testing/test-c-full.sh
```

See [.agents/standards/testing.md](.agents/standards/testing.md) for failure,
skip, firmware, and host validation responsibilities. Host checks do not prove
hardware behavior.

This is a separate Git repository. Enable its hook explicitly, once per local
working copy, from this directory:

```bash
git config core.hooksPath tools/git-hooks
```

Creating the hook does not activate it. Do not bypass hooks without explicit
user authorization.

## Execution Plans

Search existing plans under `docs/` before creating one. Future plans use
`docs/exec-plans/active/` (in progress), `pending/` (decided, not started), and
`completed/` (done or stale; do not act on unless instructed). Create these
directories as needed; do not relocate existing documents incidentally.
