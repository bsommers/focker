# Roadmap: focker

## Overview

Milestone v1.0, Hardening. focker works and is documented (commit 3619015, 71 passing integration checks). This milestone makes it trustworthy:
1. fix the known edge cases in the cutover engine
2. test the paths nobody has exercised yet
3. put sanitizers and CI behind every change
4. trim the `/proc` scanning cost

There are no new user-facing features.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [x] **Phase 1: Cutover correctness** - Fix tracee reaping, group-stop preservation, pidfd-first validation, re-sent signal reporting
- [x] **Phase 2: Test coverage** - Cover every untested path in TESTING.md, add parser unit tests
- [x] **Phase 3: Sanitizers and CI** - ASan/UBSan and analyzer builds, CI running the suite
- [x] **Phase 4: Efficiency** - Cheaper child enumeration, measured idle cost, documented limits

## Phase Details

### Phase 1: Cutover correctness
**Goal**: The four known cutover edge cases in CONCERNS.md are fixed, each with a regression test
**Depends on**: Nothing (first phase)
**Requirements**: REL-01, REL-02, REL-03, REL-04
**Success Criteria** (what must be TRUE):
  1. If a target is killed mid-cutover, `focker inspect` shows it with its real exit status
  2. If a target is SIGSTOP'd during the cutover, it is still stopped (`T`) after thaw
  3. Cutover targets are held by pidfd from validation onwards, and a test shows that a vanished target is reported, not mistaken for another process
  4. A re-sent, already-pending signal shows up in the transcript
**Plans**: 1 plan

Plans:
- [x] 01-01-PLAN.md — REL-01..04 fixes with regression tests

### Phase 2: Test coverage
**Goal**: Every gap listed in .planning/codebase/TESTING.md has a test
**Depends on**: Phase 1
**Requirements**: TST-01, TST-02, TST-03, TST-04, TST-05, TST-06
**Success Criteria** (what must be TRUE):
  1. `make test` covers `--no-drain`, `--no-suppress`, `--wait`, `logs -f`, handover exec-failure recovery, long-path sockets and limit edges
  2. `make unit` runs parser tests for procfs.c and unixdiag.c
  3. The suite still passes three runs in a row (no flakes)
**Plans**: 2 plans

Plans:
- [x] 02-01-PLAN.md — integration coverage for TST-01..05, plus the limit bugs it exposed
- [x] 02-02-PLAN.md — `make unit`: procfs fixture tests and live sock_diag (TST-06)

### Phase 3: Sanitizers and CI
**Goal**: Memory and UB errors and analyzer findings are caught automatically on every push
**Depends on**: Phase 2
**Requirements**: BLD-01, BLD-02, BLD-03
**Success Criteria** (what must be TRUE):
  1. `make asan test` passes with no sanitizer reports
  2. `make lint` exits 0
  3. The CI workflow file runs both on push, and it is verified with a local runner (`act`) or a first push
     (changed by the user: everything local, workflow left disabled; verified with `make ci`)
**Plans**: 1 plan

Plans:
- [x] 03-01-PLAN.md — make asan / lint / ci, disabled workflow in ci/

### Phase 4: Efficiency
**Goal**: Supervisor work per request and per idle tick is bounded and documented
**Depends on**: Phase 3
**Requirements**: PERF-01, PERF-02
**Success Criteria** (what must be TRUE):
  1. Child enumeration avoids the full `/proc` scan when `/proc/PID/task/*/children` exists, with identical results (tested both ways)
  2. Idle supervisor CPU and wakeups are measured and written into docs/ARCHITECTURE.md
  3. Member-history and held-fd limits are documented in USAGE and LIMITATIONS
**Plans**: 1 plan

Plans:
- [x] 04-01-PLAN.md — task/*/children child lookup, idle-cost measurement, limits docs (steps in docs/superpowers/plans/2026-10-20-efficiency.md)

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3 → 4

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Cutover correctness | 1/1 | Complete | 2026-10-20 |
| 2. Test coverage | 2/2 | Complete | 2026-10-20 |
| 3. Sanitizers and CI | 1/1 | Complete | 2026-10-20 |
| 4. Efficiency | 1/1 | Complete | 2026-10-20 |
