# Phase 4 Efficiency Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the supervisor's child enumeration avoid a full `/proc` scan, and measure and document what an idle supervisor costs.

**Architecture:** `proc_children()` in `src/procfs.c` first reads `/proc/PID/task/*/children` (one file per thread, space-separated child PIDs), merges them, sorts them and drops duplicates. It falls back to the existing PPid scan only when those files don't exist (kernels without `CONFIG_PROC_CHILDREN`). A measurement script samples the idle supervisor's `/proc/PID/schedstat` and context switches before and after, and the numbers go into `docs/ARCHITECTURE.md`.

**Tech Stack:** C11 + glibc, bash, the existing `tests/unit.c` and `tests/run.sh` harnesses.

**Spec:** `.planning/REQUIREMENTS.md` (PERF-01, PERF-02), `.planning/ROADMAP.md` Phase 4, `.planning/phases/04-efficiency/04-CONTEXT.md`

## Global Constraints

- Linux 5.6+, glibc 2.32+, no new dependencies.
- PERF-01: "Child enumeration uses `/proc/PID/task/*/children` when available, and falls back to the full `/proc` scan"
- Roadmap: "identical results (tested both ways)"
- PERF-02: "The idle supervisor's wakeup and CPU cost is measured and documented. The member-history limit is documented in USAGE and LIMITATIONS"
- `make ci` (lint + test + asan) must still pass. Transcript and CLI output don't change.

## Review Focus

- A multithreaded parent whose children were forked by different threads: the union of every thread's file is needed, not just the leader's. (Unit fixture: children split across `task/100` and `task/101`.)
- A thread that exits between `opendir(task)` and the open of its `children` file: skip it, don't fall back to the scan or fail. (Covered by the code path that ignores ENOENT after at least one file was read.)
- A process with more children than `max`: return the `max` lowest PIDs, sorted, so `cutover --from`'s `MAX_PIDS + 1` overflow check still works. (Unit: `proc_children(100, v, 2)` gives `{101, 102}`.)
- A parent PID that doesn't exist: return 0, as the scan did. (Unit: `proc_children(999, ...) == 0`.)
- Duplicate PIDs: the per-thread lists don't overlap in practice. Dedupe anyway, since a reparent between two file reads can list the same child twice.

---

### Task 1: Idle-cost measurement script and baseline

**Files:**
- Create: `tests/idle-cost.sh`
- Modify: `Makefile` (target `idle-cost`; add the script to `LINT_SH`)

**Interfaces:**
- Produces: `tests/idle-cost.sh [SECONDS]` (default 20). It prints one line, `idle supervisor over Ns: CPU X us/s, Y wakeups/s (Z processes on the system)`, and exits 0. `make idle-cost` runs it against `build/`.

- [x] **Step 1:** Write the script. It uses a private `FOCKER_RUNTIME_DIR` and `focker run -d -- sleep 600`, sleeps 1 s, then samples field 1 of `/proc/SUP/schedstat` (ns on CPU) and `voluntary_ctxt_switches + nonvoluntary_ctxt_switches` from `/proc/SUP/status` at the start and end of the window. It stops the family afterwards and removes the dir in an EXIT trap.
- [x] **Step 2:** Run `make idle-cost` on the current code and record the baseline line in the plan's SUMMARY.
- [x] **Step 3:** `make lint`: exit 0. Commit.

### Task 2: Fast child enumeration, tested both ways

**Files:**
- Modify: `src/procfs.c` (`proc_children`), `src/focker.h`
- Test: `tests/unit.c` (`test_children_both_ways`)

**Interfaces:**
- Produces: `extern bool proc_children_scan_only;` (default `false`; when it's true, `proc_children` skips the fast path). Only the unit tests set it. `int proc_children(pid_t parent, pid_t *out, int max)` keeps its signature. It returns `min(total, max)`, the lowest PIDs, sorted ascending, without duplicates.

- [x] **Step 1: Write the failing tests** in `tests/unit.c`:
  - Fixture: `100/task/100/children` = `"102 101 "`, `100/task/101/children` = `"103 102 "` (102 listed twice); 103 and 104 have no `task/` dir.
  - `children_fast_matches_scan`: run `proc_children(100, v, 8)` with `proc_children_scan_only` false, then true. Both give `n == 3`, `{101, 102, 103}`.
  - `children_fast_path_used`: write `100/task/100/children` = `"101 "` only, with no `task/101/children`. Fast mode gives `{101}` and scan mode gives `{101, 102, 103}`. This proves the fast path is really taken. Restore the fixture afterwards.
  - `children_fallback`: `proc_children(103, v, 8) == 1 && v[0] == 104` (no task dir, so it scans).
  - `children_cap`: `proc_children(100, v, 2) == 2 && v[0] == 101 && v[1] == 102`.
  - `children_live_both_ways`: fork 3 children that `pause()`, compare both modes on `getpid()` in live `/proc`, then kill and reap them.
- [x] **Step 2:** `make unit`: FAIL (`proc_children_scan_only` undeclared, so it doesn't compile).
- [x] **Step 3: Implement** in `src/procfs.c`. `static int children_fast(pid_t parent, pid_t *buf, int cap)` returns the count, or -1 when no thread has a `children` file (ENOENT on every thread, or no task dir), which triggers the scan. Read each file with `read_small` into a 4096-byte buffer and parse with `strtol`. Collect into a local `pid_t buf[4096]`, then sort, dedupe and copy `min(n, max)` to `out`. Apply the same sort-then-truncate to the scan path, so both modes return the lowest PIDs.
- [x] **Step 4:** `make unit`: all pass. Then `make test`: `117 passed, 0 failed`.
- [x] **Step 5:** Commit.

### Task 3: Measure again and document

**Files:**
- Modify: `docs/ARCHITECTURE.md` (new §5.6 "Idle cost"), `docs/USAGE.md` (a "Limits" section: 64 targets, 128 held fds, 256 members, 256 threads), `docs/LIMITATIONS.md` (link to the USAGE limits), `.planning/codebase/CONCERNS.md` (drop the `proc_children` scan item)

- [x] **Step 1:** `make idle-cost` on the new code. Write the before and after lines, the process count and the method into ARCHITECTURE §5.6.
- [x] **Step 2:** `make ci`: exit 0. Commit.

## Outcome (2026-10-20)

All steps done. Commits: `tests: idle-cost.sh ...` (Task 1), `procfs: list children from /proc/PID/task/*/children` (Task 2), and the docs and planning commit (Task 3). Idle supervisor: 11.3 ms/s → 52.6 µs/s CPU, 1.5 → 0.95 wakeups/s (about 960 processes). `make ci` passes: 43 unit + 117 integration checks under the plain, ASan and UBSan builds.
