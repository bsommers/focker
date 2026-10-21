---
phase: 07-async-cutover
plan: 01
subsystem: supervisor
tags: [cutover, poll-loop, ptrace, signals]
requires: [06-01]
provides: [cutover_start, cutover_step, cutover_cancel, cutover_owns, CUTOVER_PENDING, busy refusals]
affects: [09-stubborn-parents]
key-files:
  created: []
  modified: [src/cutover.c, src/focker.h, src/supervisor.c, tests/run.sh, docs/USAGE.md, docs/ARCHITECTURE.md, .planning/codebase/CONCERNS.md, CHANGELOG.md]
key-decisions:
  - "only the wait for the old parents is async; validation, freeze, fd takeover and thaw stay inline (bounded)"
  - "one job at a time; cutover (with --dry-run), release, stop and handover get busy; info, members, inspect are answered"
  - "a dead target that isn't a member has its exit consumed by the job; the reaper skips the job's targets"
requirements-completed: [ASYNC-01]
completed: 2026-10-21
---

# Phase 7 Plan 01: Async cutover Summary

**The wait step of a cutover runs inside the supervisor's poll loop, so held fds keep draining and read-only requests are answered during it. Steps are in docs/superpowers/plans/2026-10-20-async-cutover.md.**

## Accomplishments
- `cutover()` became a heap job: `cutover_start` (steps 0-3), `cutover_step` (one non-blocking check; verify/thaw/bystanders or the timeout rollback), `cutover_cancel`, `cutover_owns`.
- `supervisor.c` keeps one in-flight job with its client fd and transcript, polls every 20 ms while it's set, and replies through `finish_inflight` when it ends. Mutating requests get `busy (a cutover is in progress)`. The loop never exits while a job is in flight.
- TERM/INT/HUP/QUIT during the wait cancel the job (`cutover: cancelled by SIGTERM`, rollback) before forwarding.
- 19 integration checks (`async cutover`): drains grow mid-wait; `inspect` and `ps` answer; second cutover, dry run, `release`, `handover` and `stop` are busy and `stop` leaves the family running; the waiting cutover completes; a vanished client doesn't stop it; a target killed mid-wait fails the cutover and isn't a member; SIGTERM rolls back first and leaves the target unstopped and untraced.

## Deviations / found along the way
- A SIGKILLed frozen target made its oldparent exit, so the zombie landed on the supervisor and was reported `adopted` (exit 0). Verify now adopts a target only while its pidfd says it's alive, and dead non-member targets are consumed at every step and when the job ends.
- `-fanalyzer` flagged `live[]` in `thaw_ptrace` once `ntids` came from the heap job; it gets a zero initializer.

## Verification
- `make ci` exit 0: 84 unit + 176 integration checks under the plain, ASan and UBSan builds, no sanitizer reports.
