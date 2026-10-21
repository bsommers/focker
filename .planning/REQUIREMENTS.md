# Requirements: focker

**Defined:** 2026-10-20 (v2.0)
**Core Value:** A cutover must never leave a child worse off. Either the child ends up adopted, running, with its I/O intact, or everything rolls back and it stays exactly where it was.

## v2 Requirements

Milestone v2.0, Reach: more of what a child is connected to, more ways to drive focker, and a supervisor that
keeps working during a cutover. v1.0 requirements are archived in `.planning/milestones/v1.0-REQUIREMENTS.md`.

### Fd control

- [x] **FD-02**: `focker release NAME FD` closes one held fd (for example to send EOF to a child's stdin). `inspect` no longer lists it, its drain log is closed, and releasing an unknown fd exits 1
- [x] **FD-01**: Loopback TCP connections between the old parent and a target are matched via `NETLINK_SOCK_DIAG`/`inet_diag` (IPv4 and IPv6) and taken over like unix sockets; `--dry-run` shows them as `tcp`

### Machine output

- [x] **OUT-01**: `ps --json` and `inspect --json` print one JSON document (stable keys, documented in USAGE), so scripts stop parsing columns

### Supervisor

- [x] **ASYNC-01**: A cutover runs as a non-blocking state machine inside the poll loop. During a cutover (up to `--timeout`), held fds keep draining and other read-only requests (`info`, `inspect`, `members`) are answered. A second cutover is refused with `busy`

### Reach

- [ ] **CRIU-01**: Adopt a process tree that was never started under focker, through a CRIU dump/restore wrapper run as the user. focker itself stays unprivileged; the privilege belongs to `criu` (user chose option a: the capability goes on `criu`)

### Ops (added 2026-10-21 from the docs/USE-CASES.md walkthroughs; user picked all four)

- [ ] **STUB-01**: When the cutover will send SIGTERM to an old parent that catches it (`SigCgt` in `/proc/PID/status`), `--dry-run` and the cutover transcript warn that a parent which forwards TERM and waits (like `timeout`) will make the cutover time out, and suggest `-s KILL` or `--kill-after`
- [ ] **STUB-02**: `cutover --kill-after SEC`: if an old parent is still alive SEC seconds after the first signal, it gets SIGKILL (with a transcript line) instead of the cutover timing out and rolling back
- [ ] **STUB-03**: `cutover --tree`: each target's descendants are frozen too, and signals that reach them during the cutover are swallowed like the target's, so process-group signals from the dying parent can't hit them. They stay the target's children
- [ ] **WAIT-01**: `focker wait [-t SEC] NAME PID...` blocks until every listed member has exited and exits with the last one's exit code (128+N for a signal). A PID that isn't a member exits 1; the timeout exits 124
- [ ] **PAUSE-01**: `focker freeze --ptrace NAME PID...` pauses every thread through the supervisor's ptrace, with no SIGSTOP and no stop reported to the parent. `thaw` resumes it and delivers whatever arrived meanwhile. `inspect` shows paused members, and a cutover of a paused member is refused with a hint to thaw it
- [ ] **PTY-01**: A pty master held by the old parent (`script`, `expect`, pexpect-style harnesses) is taken over as kind `pty` and drained, so the child keeps its controlling terminal through the cutover
- [ ] **PTY-02**: `cutover --new-tty` gives a target whose stdio is a terminal focker can't take (an SSH session's pty) a fresh pty whose master focker drains, re-pointing fds 0-2 by syscall injection under the ptrace freeze (reptyr's technique), so closing the original session doesn't hang up the job

## Out of Scope

| Feature | Reason |
|---------|--------|
| Non-Linux support | Built on Linux-only kernel interfaces |
| Privileged focker (root, capabilities on focker itself) | Same-uid, unprivileged model is a core constraint |
| Moving pgrp/session/cgroup automatically | Separate concern; documented as a manual step in LIMITATIONS |
| Eventfds, memfds | They survive the parent's death on their own unless it held the only far end; no demand yet (ptys moved in scope: PTY-01/02) |
| Enabling the GitHub workflow | User: "no CI on github yet". It stays in `ci/` until asked |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| FD-02 | Phase 5 | Complete |
| OUT-01 | Phase 5 | Complete |
| FD-01 | Phase 6 | Complete |
| ASYNC-01 | Phase 7 | Complete |
| CRIU-01 | Phase 8 | Pending |
| STUB-01 | Phase 9 | Pending |
| STUB-02 | Phase 9 | Pending |
| STUB-03 | Phase 9 | Pending |
| WAIT-01 | Phase 10 | Pending |
| PAUSE-01 | Phase 11 | Pending |
| PTY-01 | Phase 12 | Pending |
| PTY-02 | Phase 12 | Pending |

**Coverage:**
- v2 requirements: 12 total
- Mapped to phases: 12
- Unmapped: 0

---
*Requirements defined: 2026-10-20 for milestone v2.0*
