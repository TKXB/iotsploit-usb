# C Agent Instructions Proposal

Status: Implemented. Gate verified on Linux (6/6 CTest passed; `test_can_capture` skipped without vcan0).

## Objective

Mirror the agent instructions and supporting structure in the parent UI project (`../../`), adapting its Dart and Flutter guidance to the C library, firmware examples, and CMake tests in `iotsploit-usb`.

Reference files:

- `../../AGENTS.md`
- `../../CLAUDE.md`
- `../../.agents/README.md`
- `../../.agents/standards/`
- `../../tools/testing/test-flutter-full.sh`
- `../../tools/git-hooks/pre-commit`

## Proposed File Tree

```text
iotsploit-usb/
├── AGENTS.md                         # Authoritative C agent instructions
├── CLAUDE.md                         # Short pointer to AGENTS.md
├── .agents/
│   ├── README.md                     # Standards index
│   └── standards/
│       ├── c.md                      # C toolchain, conventions, build commands
│       ├── firmware-design.md        # Replaces UI design guidance
│       ├── integration.md            # Core, transport, board, host boundaries
│       └── testing.md                # C test gate and validation policy
└── tools/
    ├── testing/
    │   └── test-c-full.sh             # Configure → build → CTest
    └── git-hooks/
        └── pre-commit                # Invokes the C gate
```

## Proposed Content

### Core Instructions

Preserve the UI project's principles: simplicity, reuse, fixing behavior at its owner, minimal scope, and regression prevention. Make `AGENTS.md` authoritative, with `CLAUDE.md` referring to it and `.agents/README.md` indexing the standards.

Replace Flutter commands and UI component guidance with the existing C build workflow and reuse of the core, helpers, and transport glue.

### C Standards

Follow the existing CMake configuration: C99, with C11 for MSVC. Document buffer ownership, bounds, callback lifetimes, and the existing no-allocation-after-init design. Preserve existing compiler warning settings and vendored-code boundaries.

Do not introduce a formatter or reformat existing C sources as part of this change.

### Firmware Design and Integration

Keep the core independent of USB stacks, RTOS headers, and board support packages. Reuse existing helpers and transport glue. Keep board initialization and device-specific behavior in their existing owners, including firmware examples.

Document the boundaries between the core, USB and socket transports, board code, and host consumers. Retain relevant Rust host and Python test guidance for those directories.

### Test Gate

Add `tools/testing/test-c-full.sh` to configure a separate build directory, build, and run CTest, stopping on failure.

The gate will:

- Enable tests, helpers, and socket glue.
- Run existing C tests and Python transport tests registered with CTest.
- Require Python so transport coverage cannot silently disappear.
- Report hardware-dependent skips separately from successful validation.
- Preserve existing compiler warning settings and vendor boundaries.
- Fail when a required configure, build, or test step fails.

Document agent responsibilities: fix failures caused by the change, report environmental blockers, do not weaken unrelated tests, and do not bypass hooks without explicit authorization. Follow the parent's policy of using existing coverage and focused validation, adding minimal tests only for uncovered, high-risk regression paths.

Require relevant firmware builds for board-specific changes and distinguish host validation from actual hardware verification.

### Git Hook

Provide `tools/git-hooks/pre-commit` to invoke the C gate and document local setup:

```bash
git config core.hooksPath tools/git-hooks
```

Hook activation is a separate local configuration step; creating the proposed files does not activate it.

### Execution Plans

Search existing plans under `docs/` before creating a new plan. Introduce the parent project's convention for future plans:

```text
docs/exec-plans/
├── active/
├── pending/
└── completed/
```

Create those directories as plans need them. Do not relocate existing documents in this change.

## Implementation and Validation After Approval

1. Read the reference runner and hook before adapting their behavior.
2. Create the instructions and standards, checking every documented path and command against this repository.
3. Implement the C gate and hook using the existing CMake and CTest infrastructure.
4. Check shell syntax and run the C gate; report test results, skips, and any environmental blockers.
5. Review the diff for consistency, stale Flutter references, and unintended changes.

## Approval Scope

Approval is requested for the instruction files, supporting standards, C test runner, and pre-commit hook described above. Existing source files and existing plans remain outside this proposal's implementation scope unless a necessary change is identified and presented for review.
