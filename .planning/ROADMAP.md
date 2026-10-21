# Roadmap: focker

## Milestones

- ✅ **v1.0 Hardening** - Phases 1-4 (shipped 2026-10-20). See `.planning/milestones/v1.0-ROADMAP.md`
- 🚧 **v2.0 Reach** - Phases 5-8 (planned)

## Overview

Milestone v2.0, Reach. v1.0 made focker trustworthy. v2.0 makes it reach further:
1. small user-facing controls first: release one held fd, and JSON output
2. loopback TCP takeover, the most-asked-for fd kind after pipes and unix sockets
3. an async cutover, so the supervisor keeps draining and answering during a cutover
4. adopting processes that were never started under focker, via CRIU (blocked on a decision)

Phases are ordered so each one ships on its own. The riskiest refactor (async) comes after the features
whose tests it then has to keep passing.

## Phases

**Phase Numbering:**
- Integer phases continue from v1.0 (5, 6, ...)
- Decimal phases (5.1, 5.2): Urgent insertions (marked with INSERTED)

- [x] **Phase 5: Fd control and JSON** - `focker release`, `ps --json`, `inspect --json`
- [ ] **Phase 6: Loopback TCP takeover** - inet_diag peer matching for TCP between old parent and target
- [ ] **Phase 7: Async cutover** - non-blocking cutover inside the poll loop
- [ ] **Phase 8: CRIU adoption** - adopt trees not started under focker, via a capability-granted `criu`

## Phase Details

### Phase 5: Fd control and JSON
**Goal**: Users can let go of a single held fd, and scripts can read focker's state without parsing columns
**Depends on**: v1.0
**Requirements**: FD-02, OUT-01
**Success Criteria** (what must be TRUE):
  1. After `focker release NAME FD` on a held `pipe-w`, the child reads EOF on its stdin (integration test with a child that exits on EOF)
  2. Releasing a drained read end closes its log, and `inspect` stops listing it; an unknown FD exits 1
  3. `ps --json` and `inspect --json` parse with `python3 -m json.tool`, and their keys are documented in USAGE
**Plans**: 1 plan

Plans:
- [x] 05-01-PLAN.md — sb_json_str, inspect/ps --json, focker release (steps in docs/superpowers/plans/2026-10-20-fd-control-and-json.md)

### Phase 6: Loopback TCP takeover
**Goal**: A child talking to its old parent over loopback TCP keeps its connection through a cutover
**Depends on**: Phase 5
**Requirements**: FD-01
**Success Criteria** (what must be TRUE):
  1. A fixture where the old parent and the child share a 127.0.0.1 and a ::1 connection: after the cutover the child's socket is still connected and its output is drained to a log
  2. `--dry-run` lists the connection as `tcp` with both endpoints
  3. Non-loopback and listening sockets are never taken
**Plans**: 1 plan

Plans:
- [ ] 06-01-PLAN.md — inetdiag.c, tcp takeover in plan_fds, -T4/-T6 fixture (steps in docs/superpowers/plans/2026-10-20-loopback-tcp.md)

### Phase 7: Async cutover
**Goal**: The supervisor never stops draining or answering read-only requests while a cutover is in flight
**Depends on**: Phase 6
**Requirements**: ASYNC-01
**Success Criteria** (what must be TRUE):
  1. During a `--wait -t 5` cutover, a second child's pipe keeps draining (its log grows) and `inspect` answers
  2. A second cutover during the first gets `busy` and exit 1
  3. Every v1.0 cutover check still passes, under `make ci`
**Plans**: TBD

### Phase 8: CRIU adoption
**Goal**: A process tree started outside focker can be brought under a family
**Depends on**: Phase 7
**Requirements**: CRIU-01
**Decision** (user, 2026-10-20): option (a). focker runs a `criu` that the user has granted `CAP_CHECKPOINT_RESTORE` (`setcap cap_checkpoint_restore+eip $(command -v criu)`); focker itself gets no privilege
**Success Criteria** (what must be TRUE):
  1. `focker adopt-external NAME PID` (name settled in planning) dumps the tree with criu and restores it under the family's supervisor, so its processes become members
  2. Without a capable criu it exits 1 with a message naming the `setcap` command, and changes nothing
  3. An integration test runs when a capable criu is present and is reported as `skip` otherwise
**Plans**: TBD

## Progress

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1-4 | v1.0 | 5/5 | Complete | 2026-10-20 |
| 5. Fd control and JSON | v2.0 | 1/1 | Complete | 2026-10-20 |
| 6. Loopback TCP takeover | v2.0 | 0/TBD | Not started | - |
| 7. Async cutover | v2.0 | 0/TBD | Not started | - |
| 8. CRIU adoption | v2.0 | 0/TBD | Not started | - |
