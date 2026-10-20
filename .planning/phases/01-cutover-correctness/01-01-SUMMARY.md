---
phase: 01-cutover-correctness
plan: 01
subsystem: cutover
tags: [ptrace, pidfd, signals]
requires: []
provides: [pidfd-pinned targets, reaper-owned exit status, kept job-control stops, already-pending signal report]
affects: [02-test-coverage]
tech-stack:
  added: []
  patterns: [waitid WNOWAIT peek before reaping a tracee]
key-files:
  created: []
  modified: [src/cutover.c, examples/ticker.c, tests/run.sh, docs/ARCHITECTURE.md, docs/LIMITATIONS.md, docs/USAGE.md]
key-decisions:
  - "Group-stops in the swallow loop stay on PTRACE_CONT, not PTRACE_LISTEN"
  - "FOCKER_TEST_STALL_MS test hook to reach the validate-to-freeze window"
requirements-completed: [REL-01, REL-02, REL-03, REL-04]
duration: 1 session
completed: 2026-10-20
---

# Phase 1 Plan 01: Cutover correctness Summary

**The cutover now pins targets by pidfd, leaves adopted exits to the reaper, keeps job-control stops and reports signals it can't swallow.**

## Accomplishments
- REL-01: `wait_tracee()` peeks with `waitid(WNOWAIT)` and leaves an adopted leader's zombie for `reap_children()`, so `inspect` shows `exited 7` instead of a stale `running`.
- REL-02: SIGSTOP/TSTP/TTIN/TTOU are never swallowed. The transcript reports `it was kept, so PID will stop`, and the target is in state `T` after the thaw.
- REL-03: targets are pinned with `pidfd_open` before any `/proc` read and re-checked before and after the freeze (`PID exited before it could be frozen`).
- REL-04: signals pending both before the freeze and at thaw are named in the transcript.

## Deviations from Plan
- REL-02's planned fix (`PTRACE_LISTEN`) was wrong. Tested with a pdeathsig plus SIGSTOP, LISTEN failed 3/3 runs: the pending signal was never dequeued, so it wasn't swallowed, and the next SIGCONT killed the child with it. `PTRACE_CONT` is kept, because `PTRACE_DETACH` re-arms the group-stop. The actual bug was that SIGSTOP counted as a fresh signal and got swallowed. The test now guards against the LISTEN change.

## Verification
- Every new check failed before its fix (REL-01 also re-checked by disabling only the new branch).
- `make test`: 81 passed, 0 failed, five runs in a row.

## Issues Found
- `wait_for test "$(...)"` in `tests/run.sh` evaluates once and never retries (used in `start()`). Logged in CONCERNS.md for Phase 2.
