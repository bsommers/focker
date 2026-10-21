# Requirements: focker

**Defined:** 2026-10-20
**Core Value:** A cutover must never leave a child worse off. Either the child ends up adopted, running, with its I/O intact, or everything rolls back and it stays exactly where it was.

## v1 Requirements

Milestone v1.0, Hardening. Each requirement maps to a roadmap phase.

### Reliability

- [x] **REL-01**: A target that dies while ptrace-attached during a cutover still has its exit status recorded in `focker inspect` (thaw must not silently reap it)
- [x] **REL-02**: A job-control stop that arrives for a target during the cutover is preserved after thaw (not swallowed; PTRACE_CONT kept, since PTRACE_LISTEN would block swallowing)
- [x] **REL-03**: Cutover targets are pinned with a pidfd before validation, so a recycled PID can't be frozen or adopted by mistake
- [x] **REL-04**: A signal that was already pending before the freeze and is re-sent during the cutover is reported in the transcript, not silently left to coalesce

### Testing

- [x] **TST-01**: Integration tests cover `--no-drain`, `--no-suppress` and `--wait` (successful case)
- [x] **TST-02**: Integration tests cover `logs -f` termination when the family exits
- [x] **TST-03**: Integration tests cover handover exec-failure recovery (family keeps answering, held fds still drained)
- [x] **TST-04**: Integration tests cover the long-runtime-path `control_addr` fallback (state dir longer than 108 bytes)
- [x] **TST-05**: Integration tests cover the limit edges: 64 targets, `MAX_STOLEN` held fds, member-history eviction
- [x] **TST-06**: Unit tests exercise `procfs.c` parsers and `unixdiag.c` against fixture data and live sockets

### Build & CI

- [x] **BLD-01**: `make asan` builds with ASan+UBSan, and the full suite passes under it with no reports (two builds: ASan+LSan, then UBSan)
- [x] **BLD-02**: `gcc -fanalyzer` (or cppcheck) runs clean, or has documented suppressions, via `make lint`
- [x] **BLD-03**: A CI workflow runs `make test` and `make asan` on every push (written and checked locally with `make ci`; disabled at the user's request until there is a GitHub remote)

### Efficiency

- [x] **PERF-01**: Child enumeration uses `/proc/PID/task/*/children` when available, and falls back to the full `/proc` scan
- [x] **PERF-02**: The idle supervisor's wakeup and CPU cost is measured and documented. The member-history limit is documented in USAGE and LIMITATIONS

## v2 Requirements

Deferred and tracked, but not in the current roadmap.

### Supervisor

- **ASYNC-01**: A cutover runs as a non-blocking state machine inside the poll loop
- **OUT-01**: `ps` and `inspect` support `--json`

### Fd coverage

- **FD-01**: Loopback TCP peers are matched via inet_diag
- **FD-02**: `focker release NAME FD` closes a held fd (e.g. to send EOF to a child's stdin)

### Reach

- **CRIU-01**: Adopt processes not started under focker via a CRIU dump/restore wrapper

## Out of Scope

| Feature | Reason |
|---------|--------|
| Non-Linux support | Built on Linux-only kernel interfaces |
| Privileged operation (root, capabilities) | Same-uid, unprivileged model is a core constraint |
| Moving pgrp/session/cgroup automatically | Separate concern; documented as a manual step in LIMITATIONS |

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| REL-01 | Phase 1 | Complete |
| REL-02 | Phase 1 | Complete |
| REL-03 | Phase 1 | Complete |
| REL-04 | Phase 1 | Complete |
| TST-01 | Phase 2 | Complete |
| TST-02 | Phase 2 | Complete |
| TST-03 | Phase 2 | Complete |
| TST-04 | Phase 2 | Complete |
| TST-05 | Phase 2 | Complete |
| TST-06 | Phase 2 | Complete |
| BLD-01 | Phase 3 | Complete |
| BLD-02 | Phase 3 | Complete |
| BLD-03 | Phase 3 | Complete (disabled) |
| PERF-01 | Phase 4 | Complete |
| PERF-02 | Phase 4 | Complete |

**Coverage:**
- v1 requirements: 15 total
- Mapped to phases: 15
- Unmapped: 0

---
*Requirements defined: 2026-10-20*
*Last updated: 2026-10-20 after brownfield onboarding*
