---
phase: 02-test-coverage
plan: 02
subsystem: tests
tags: [unit, procfs, sock_diag]
requires: []
provides: [make unit]
affects: [03-sanitizers-ci]
key-files:
  created: [tests/unit.c]
  modified: [src/procfs.c, src/focker.h, Makefile]
key-decisions:
  - "proc_root global instead of a mocking layer"
requirements-completed: [TST-06]
completed: 2026-10-20
---

# Phase 2 Plan 02: Parser unit tests Summary

**`make unit` runs 35 checks of the /proc readers against a fixture tree and of sock_diag against a live socketpair. `make test` runs it first.**

## Accomplishments
- `procfs.c` builds every path from `proc_root` (default `/proc`). No behavior change.
- The fixture tree covers status fields (including `TracerPid:` next to `PPid:`), the cmdline→`[comm]` fallback, truncation, non-numeric entries, child/descendant/thread listing, fd links with fdinfo access modes (and a missing fdinfo), numeric fd sorting, and the `ShdPnd`|`SigPnd` OR.

## Verification
- These describe how the parsers already behave, so there was nothing to see fail beforehand. Two deliberate breaks (dropping the fd sort, dropping `ShdPnd`) were each caught (5 and 1 failures).
