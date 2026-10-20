---
phase: 02-test-coverage
plan: 01
subsystem: tests
tags: [integration, limits]
requires: [01-01]
provides: [coverage for options, logs -f, handover failure, long paths, limits, SIGINT]
affects: [03-sanitizers-ci]
key-files:
  modified: [tests/run.sh, src/cutover.c, examples/oldparent.c]
key-decisions:
  - "Limits are refused up front instead of partially applied"
requirements-completed: [TST-01, TST-02, TST-03, TST-04, TST-05]
completed: 2026-10-20
---

# Phase 2 Plan 01: Integration coverage Summary

**36 new integration checks (81 → 117), and three limit bugs fixed that the new checks exposed.**

## Accomplishments
- `start()` re-evaluates its waits (the old `wait_for test "$(...)"` ran once).
- Checks for `--no-drain`, `--no-suppress`, `--wait`, `logs -f`, handover exec failure, a 120-character runtime dir, the 64/65-target edge, the 128-fd limit and slot reuse, the 256-member history, SIGINT forwarded from `kill`, and Ctrl-C from a pty.

## Bugs found and fixed (each check failed first)
- `cutover --from` a parent with 65+ children froze only 64. The rest were orphaned unfrozen, without fd takeover. Now refused.
- Held-fd slots were never reclaimed, so a family could take only 128 fds in its lifetime. They are compacted at the start of each cutover.
- A cutover past 128 held fds went ahead and left the extra children to die of SIGPIPE. It is now refused before anything is frozen, `--dry-run` included.

## Issues Found (not fixed)
- `handover` replies success before `execve`, so a failed exec still exits 0 for the caller (CONCERNS.md).

## Verification
- `make test`: 35 unit + 117 integration checks passed, three runs in a row (about 19 s, up from 8, mostly the limit tests).
