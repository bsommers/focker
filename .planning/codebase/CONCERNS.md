---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# Codebase Concerns

**Analysis Date:** 2026-10-20

## Tech Debt

- **Synchronous request handling.** `handle_client()` runs a whole cutover inside the poll loop, so other drains stall for its duration. That's fine at millisecond scale, but a long `--wait -t 3600` blocks the family's drains and other clients. Fix: run a cutover as an incremental state machine inside the loop. (`src/supervisor.c` `handle_client`, `src/cutover.c` `cutover`)
- **Static buffers in `cutover.c`** (`plan`, `cf`/`pf` fd tables) and `signame()`/`cmd_of()` rotating buffers are not reentrant. That's safe today only because the supervisor is single-threaded.
- **Bystander `old_ppid` is best-effort** when several parents are cut at once (`src/cutover.c` step 6).
- **The member table forgets the oldest dead members at 256 entries.** Long-lived families lose history.

## Known Bugs / Edge Cases

- **Reaper vs tracee:** if a target dies while ptrace-attached and is already reparented to focker, `waitpid(tid, __WALL)` inside thaw may reap it, so the main reaper never records its exit status. (`thaw_ptrace`)
- **ptrace group-stop handling:** in the swallow loop, a `PTRACE_EVENT_STOP` caused by a real group-stop is continued with `PTRACE_CONT`, which overrides a job-control stop that arrived mid-cutover. `PTRACE_LISTEN` would preserve it.
- **Pending-signal diff can miss repeats.** A signal that was already pending before the freeze and sent again during the cutover isn't "fresh", so it isn't swallowed (standard signals coalesce).
- **`proc_children` scans all of `/proc`**: O(number of processes) per call. It's called several times per request and once per loop iteration through `live_children()` (once a second, or per event).

## Security Considerations

- `pidfd_getfd` and ptrace are limited to descendants by Yama scope 1. focker adds no checks beyond descendant validation (`proc_is_descendant`), and a PID could be recycled between validation and use. The pidfd is opened after validation, so there is a small TOCTOU window for the freeze target. Opening a pidfd first and validating through it would close that.
- The control socket trusts any same-uid peer, so any process of that user can trigger cutovers or handovers.

## Missing Features (from docs/LIMITATIONS.md)

- TCP/UDP peer matching (`inet_diag`), pty handling, cgroup migration, pgrp/session moves.
- A way to release a single held fd (e.g. to send EOF to a child's stdin) without a handover.
- CRIU integration for processes not started under focker.
- Machine-readable output (`--json`) for `ps`/`inspect`.

## Test Coverage Gaps

See TESTING.md "Gaps". The highest-risk untested paths are the handover exec-failure recovery and the long-path `control_addr` fallback.

---

*Concerns audit: 2026-10-20*
