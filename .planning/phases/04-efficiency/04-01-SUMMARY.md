---
phase: 04-efficiency
plan: 01
subsystem: procfs
tags: [performance, measurement]
requires: [03-01]
provides: [task/*/children child lookup, make idle-cost, limits table]
affects: []
key-files:
  created: [tests/idle-cost.sh, docs/superpowers/plans/2026-10-20-efficiency.md]
  modified: [src/procfs.c, src/focker.h, tests/unit.c, Makefile, docs/ARCHITECTURE.md, docs/USAGE.md, docs/LIMITATIONS.md, README.md]
key-decisions:
  - "Fast path everywhere; fall back only when no children file exists"
  - "Both modes return the lowest max pids, sorted and deduped"
requirements-completed: [PERF-01, PERF-02]
completed: 2026-10-20
---

# Phase 4 Plan 01: Efficiency Summary

**Child lookup reads `/proc/PID/task/*/children`. An idle supervisor's CPU dropped about 215× (11.3 ms/s → 52.6 µs/s with ~960 processes).**

## Accomplishments
- `proc_children`: per-thread `children` files merged, sorted and deduped, with the full scan as fallback. The scan path now also returns the lowest pids when capped (before, it returned an arbitrary subset).
- 8 new unit checks compare both modes: fixture (two threads, a duplicate, the cap, fallback, a missing parent, proof that the fast path is taken) and live `/proc` (3 forked children).
- `make idle-cost`, with before/after numbers in docs/ARCHITECTURE.md §5.6, and a limits table in docs/USAGE.md linked from LIMITATIONS.

## Verification
- Baseline (two runs): 11206 and 11347 µs/s, 1.35 and 1.65 wakeups/s. After (two runs): 52.6 µs/s, 0.95 wakeups/s.
- `make ci` exit 0: 43 unit + 117 integration under the plain, ASan and UBSan builds; no sanitizer reports.
