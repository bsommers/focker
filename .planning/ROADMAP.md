# Roadmap: focker

## Milestones

- ✅ **v1.0 Hardening** - Phases 1-4 (shipped 2026-10-20). See `.planning/milestones/v1.0-ROADMAP.md`
- 🚧 **v2.0 Reach** - Phases 5-12 (in progress)

## Overview

Milestone v2.0, Reach. v1.0 made focker trustworthy. v2.0 makes it reach further:
1. small user-facing controls first: release one held fd, and JSON output
2. loopback TCP takeover, the most-asked-for fd kind after pipes and unix sockets
3. an async cutover, so the supervisor keeps draining and answering during a cutover
4. adopting processes that were never started under focker, via CRIU (option a: a capability-granted `criu`)
5. ops gaps found by the docs/USE-CASES.md walkthroughs (added 2026-10-21): stubborn parents, `focker wait`,
   an invisible pause, and pty takeover

Phases are ordered so each one ships on its own. The riskiest refactor (async) comes after the features
whose tests it then has to keep passing.

## Phases

**Phase Numbering:**
- Integer phases continue from v1.0 (5, 6, ...)
- Decimal phases (5.1, 5.2): Urgent insertions (marked with INSERTED)

- [x] **Phase 5: Fd control and JSON** - `focker release`, `ps --json`, `inspect --json`
- [x] **Phase 6: Loopback TCP takeover** - inet_diag peer matching for TCP between old parent and target
- [ ] **Phase 7: Async cutover** - non-blocking cutover inside the poll loop
- [ ] **Phase 8: CRIU adoption** - adopt trees not started under focker, via a capability-granted `criu`
- [ ] **Phase 9: Stubborn parents** - warn about TERM-catching parents, `--kill-after`, `--tree`
- [ ] **Phase 10: focker wait** - block until members exit and return their exit code
- [ ] **Phase 11: Invisible pause** - `freeze --ptrace`, a pause neither the job nor its parent sees
- [ ] **Phase 12: Pty takeover** - take a held pty master; give a terminal-bound job a fresh pty

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
- [x] 06-01-PLAN.md — inetdiag.c, tcp takeover in plan_fds, -T4/-T6 fixture (steps in docs/superpowers/plans/2026-10-20-loopback-tcp.md)

### Phase 7: Async cutover
**Goal**: The supervisor never stops draining or answering read-only requests while a cutover is in flight
**Depends on**: Phase 6
**Requirements**: ASYNC-01
**Success Criteria** (what must be TRUE):
  1. During a `--wait -t 5` cutover, a second child's pipe keeps draining (its log grows) and `inspect` answers
  2. A second cutover during the first gets `busy` and exit 1
  3. Every v1.0 cutover check still passes, under `make ci`
**Plans**: 1 plan

Plans:
- [ ] 07-01-PLAN.md — cutover as a job, stepped from the poll loop, busy refusals, reaper/signal safety (steps in docs/superpowers/plans/2026-10-20-async-cutover.md)

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

### Phase 9: Stubborn parents
**Goal**: A cutover against a parent that traps, forwards or ignores TERM succeeds or says why up front
**Depends on**: Phase 7 (the escalation runs in the async wait step)
**Requirements**: STUB-01, STUB-02, STUB-03
**Success Criteria** (what must be TRUE):
  1. `--dry-run` against `timeout 30 CHILD` warns that the old parent catches SIGTERM and suggests `-s KILL` / `--kill-after`
  2. `cutover --kill-after 1` against a TERM-ignoring parent adopts the child within about 1 s, and the transcript says SIGKILL was sent
  3. With `--tree`, a target's own child survives a process-group SIGTERM sent by the dying parent; without `--tree` the existing behavior is unchanged
**Plans**: TBD

### Phase 10: focker wait
**Goal**: Scripts can wait for an adopted job and use its exit status
**Depends on**: Phase 7
**Requirements**: WAIT-01
**Success Criteria** (what must be TRUE):
  1. `focker wait NAME PID` after the rescue-from-timeout walkthrough exits 3 when the job exits 3
  2. A signal death exits 128+N, a non-member exits 1, and `-t` expiry exits 124
**Plans**: TBD

### Phase 11: Invisible pause
**Goal**: A snapshot pause that neither the job nor its parent can observe
**Depends on**: Phase 7 (tracee events and the reaper)
**Requirements**: PAUSE-01
**Success Criteria** (what must be TRUE):
  1. During `freeze --ptrace`, the job's file stops growing, its parent's `waitpid(WUNTRACED)` reports nothing, and `inspect` shows it paused
  2. A signal sent while paused is delivered after `thaw`; a cutover of a paused member is refused with a hint
**Plans**: TBD

### Phase 12: Pty takeover
**Goal**: Jobs tied to a terminal can change parents or outlive their session too
**Depends on**: Phase 7
**Requirements**: PTY-01, PTY-02
**Success Criteria** (what must be TRUE):
  1. A child of `script -q -c CMD /dev/null` keeps running and its terminal output is drained after `script` is cut over (kind `pty`)
  2. `cutover --new-tty` on a job writing to a test pty moves fds 0-2 to a focker-held pty; hanging up the original pty no longer reaches the job
  3. `--dry-run` shows both plans; any failure in the syscall injection rolls back
**Plans**: TBD

## Progress

| Phase | Milestone | Plans Complete | Status | Completed |
|-------|-----------|----------------|--------|-----------|
| 1-4 | v1.0 | 5/5 | Complete | 2026-10-20 |
| 5. Fd control and JSON | v2.0 | 1/1 | Complete | 2026-10-20 |
| 6. Loopback TCP takeover | v2.0 | 1/1 | Complete | 2026-10-20 |
| 7. Async cutover | v2.0 | 0/1 | Planned | - |
| 8. CRIU adoption | v2.0 | 0/TBD | Not started | - |
| 9. Stubborn parents | v2.0 | 0/TBD | Not started | - |
| 10. focker wait | v2.0 | 0/TBD | Not started | - |
| 11. Invisible pause | v2.0 | 0/TBD | Not started | - |
| 12. Pty takeover | v2.0 | 0/TBD | Not started | - |
