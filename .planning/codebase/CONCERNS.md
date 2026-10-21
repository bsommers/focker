---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# Codebase Concerns

**Analysis Date:** 2026-10-20

## Tech Debt

- ~~Synchronous request handling~~: fixed in Phase 7 (ASYNC-01). The wait for the old parents is a job stepped from the poll loop; drains and read-only requests carry on, and mutating requests get `busy`. Validation, freeze, fd takeover and thaw are still inline (bounded: at most 2 s for `--freeze signal`).
- **Static buffers in `cutover.c`** (`plan`, `cf`/`pf` fd tables) and `signame()`/`cmd_of()` rotating buffers are not reentrant. That's safe today only because the supervisor is single-threaded.
- **Bystander `old_ppid` is best-effort** when several parents are cut at once (`src/cutover.c` step 6).
- **The member table forgets the oldest dead members at 256 entries.** Long-lived families lose history.

## Known Bugs / Edge Cases

- ~~Reaper vs tracee~~, ~~group-stop handling~~, ~~pending-signal repeats~~: fixed in Phase 1 (REL-01, REL-02, REL-04). The group-stop concern was misdiagnosed: the real bug was that SIGSTOP counted as a fresh signal and was swallowed. `PTRACE_CONT` is correct, and `PTRACE_LISTEN` would break swallowing (see docs/ARCHITECTURE.md §4.4). A re-sent pending signal is still undetectable (a kernel limit), and is now reported.
- ~~`wait_for test "$(...)"` in `start()`~~: fixed in Phase 2.
- ~~`--from` silently capped at 64 children~~, ~~held-fd slots never reused~~, ~~a cutover past 128 held fds went ahead without the extra fds~~: fixed in Phase 2.
- ~~The test cleanup misses orphans of a crashed supervisor~~: fixed (cleanup also kills by session). Was: `tests/run.sh` `cleanup()` kills `pkill -P <supervisor>` and the supervisor. If a supervisor crashed (seen with deliberately planted sanitizer bugs), its children were already reparented to the user's subreaper, so they survive the run. Killing by session (`pkill -s <supervisor pid>`, since a detached supervisor is its session's leader) would catch them.
- ~~`handover` reports success before `execve`~~: fixed (the `exec` reply waits for EOF or `err`). Was: the reply is sent first (the socket is torn down for the exec), so if the exec fails, the client has already printed success and exited 0. The family recovers, and `focker.log` says `handover to X failed`, but the caller doesn't know.
- ~~`proc_children` scans all of `/proc`~~: fixed in Phase 4 (`task/*/children`; idle cost 11.3 ms/s → 52.6 µs/s). `proc_list` (used by `tree`) still scans everything, but only per CLI call.

## Security Considerations

- `pidfd_getfd` and ptrace are limited to descendants by Yama scope 1. focker adds no checks beyond descendant validation (`proc_is_descendant`). Since Phase 1 (REL-03), targets are pinned with a pidfd before validation and re-checked before and after the freeze, which closes the recycled-PID window for targets. `--from` is still resolved by PID.
- The control socket trusts any same-uid peer, so any process of that user can trigger cutovers or handovers.

## Missing Features (from docs/LIMITATIONS.md)

- TCP/UDP peer matching (`inet_diag`), pty handling, cgroup migration, pgrp/session moves.
- ~~A way to release a single held fd~~: `focker release` (Phase 5).
- ~~Drains after a request used stale poll indices~~: fixed in Phase 5. A cutover or `release` can close or move held-fd slots, and drained fds are blocking, so a stale index could have blocked the supervisor in `read()`. The loop now skips draining in any round that served a request.
- CRIU integration for processes not started under focker.
- ~~Machine-readable output~~: `ps --json`, `inspect --json` (Phase 5).

## Test Coverage Gaps

See TESTING.md "Gaps". The highest-risk untested paths are the handover exec-failure recovery and the long-path `control_addr` fallback.

---

*Concerns audit: 2026-10-20*
