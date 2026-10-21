# Requirements: focker

**Defined:** 2026-10-20 (v2.0)
**Core Value:** A cutover must never leave a child worse off. Either the child ends up adopted, running, with its I/O intact, or everything rolls back and it stays exactly where it was.

## v2 Requirements

Milestone v2.0, Reach: more of what a child is connected to, more ways to drive focker, and a supervisor that
keeps working during a cutover. v1.0 requirements are archived in `.planning/milestones/v1.0-REQUIREMENTS.md`.

### Fd control

- [ ] **FD-02**: `focker release NAME FD` closes one held fd (for example to send EOF to a child's stdin). `inspect` no longer lists it, its drain log is closed, and releasing an unknown fd exits 1
- [ ] **FD-01**: Loopback TCP connections between the old parent and a target are matched via `NETLINK_SOCK_DIAG`/`inet_diag` (IPv4 and IPv6) and taken over like unix sockets; `--dry-run` shows them as `tcp`

### Machine output

- [ ] **OUT-01**: `ps --json` and `inspect --json` print one JSON document (stable keys, documented in USAGE), so scripts stop parsing columns

### Supervisor

- [ ] **ASYNC-01**: A cutover runs as a non-blocking state machine inside the poll loop. During a cutover (up to `--timeout`), held fds keep draining and other read-only requests (`info`, `inspect`, `members`) are answered. A second cutover is refused with `busy`

### Reach

- [ ] **CRIU-01**: Adopt a process tree that was never started under focker, through a CRIU dump/restore wrapper run as the user. focker itself stays unprivileged; the privilege belongs to `criu` (user chose option a: the capability goes on `criu`)

## Out of Scope

| Feature | Reason |
|---------|--------|
| Non-Linux support | Built on Linux-only kernel interfaces |
| Privileged focker (root, capabilities on focker itself) | Same-uid, unprivileged model is a core constraint |
| Moving pgrp/session/cgroup automatically | Separate concern; documented as a manual step in LIMITATIONS |
| Ptys, eventfds, memfds | They survive the parent's death on their own unless it held the only far end; no demand yet |
| Enabling the GitHub workflow | User: "no CI on github yet". It stays in `ci/` until asked |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| FD-02 | Phase 5 | Pending |
| OUT-01 | Phase 5 | Pending |
| FD-01 | Phase 6 | Pending |
| ASYNC-01 | Phase 7 | Pending |
| CRIU-01 | Phase 8 | Pending |

**Coverage:**
- v2 requirements: 5 total
- Mapped to phases: 5
- Unmapped: 0

---
*Requirements defined: 2026-10-20 for milestone v2.0*
