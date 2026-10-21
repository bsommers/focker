---
gsd_state_version: '1.0'
milestone: v2.0
milestone_name: Reach
status: planning
progress:
  total_phases: 4
  completed_phases: 2
  total_plans: 2
  completed_plans: 2
  percent: 50
---

# Project State

## Project Reference

See: .planning/PROJECT.md (updated 2026-10-20)

**Core value:** A cutover must never leave a child worse off. Either the child ends up adopted, running, with its I/O intact, or everything rolls back and it stays exactly where it was.
**Current focus:** Milestone v2.0 Reach, Phase 7: Async cutover

## Current Position

Milestone: v2.0 Reach (v1.0 Hardening shipped 2026-10-20, see .planning/MILESTONES.md)
Phase: 7 (Async cutover), of phases 5-8
Plan: 0 of TBD in current phase
Status: Not planned
Last activity: 2026-10-20 — Phase 6 complete (loopback TCP takeover, FD-01)

Progress: [█████░░░░░] 50% (v2.0)

## Performance Metrics

**Velocity:**
- Total plans completed: 7
- Average duration: -
- Total execution time: 0 hours

**By Phase:**

| Phase | Plans | Total | Avg/Plan |
|-------|-------|-------|----------|
| 1 | 1 | 1 | 1 |
| 2 | 2 | 2 | 1 |
| 3 | 1 | 1 | 1 |
| 4 | 1 | 1 | 1 |
| 5 | 1 | 1 | 1 |
| 6 | 1 | 1 | 1 |

*Updated after each plan completion*

## Accumulated Context

### Decisions

Decisions are logged in the PROJECT.md Key Decisions table.

- [Onboarding]: First milestone is Hardening (user choice). The async supervisor, more fd kinds and CRIU are deferred to v2.
- [Onboarding]: GSD artifacts were written by hand. GSD Core is not installed for Claude Code (user choice: artifacts only).
- [Phase 1]: Keep PTRACE_CONT for group-stops in the swallow loop. PTRACE_LISTEN blocks swallowing of still-pending signals, and PTRACE_DETACH re-arms the stop anyway.
- [Phase 1]: `FOCKER_TEST_STALL_MS` is a test-only hook in cutover.c (sleep between validation and freeze).
- [Phase 2]: Limits are enforced before freezing: `--from` over 64 children and cutovers past 128 held fds are refused, not partially applied.
- [Phase 2]: `procfs.c` reads from `proc_root` so unit tests can use a fixture tree. The Ctrl-C test needs python3 (skipped without it).
- [Phase 3]: User: "do phase 3 all locally, no CI on github yet. if you build github CI, leave it disabled." The workflow is `ci/github-actions.yml`, outside `.github/workflows/`.
- [Phase 3]: ASan and UBSan are separate builds, because UBSan ignores `log_path` in a combined build.
- [Phase 6]: TCP pairing compares addresses in IPv6 form (v4 as ::ffff:a.b.c.d) so dual-stack sockets pair with v4 ones; a v6 dump failing with ENOENT/EAFNOSUPPORT/EINVAL means no v6 sockets.
- [Phase 4]: Step-level plan kept in superpowers format (`docs/superpowers/plans/2026-10-20-efficiency.md`) at the user's request; GSD phase files point to it.

### Pending Todos

None yet.

### Blockers/Concerns

- [Decision 2026-10-20] Phase 8 (CRIU-01): user chose (a), a capability-granted `criu`; focker stays unprivileged.

- The `/gsd-*` slash commands are unavailable in Claude Code until gsd-core is installed for it (`npx @opengsd/gsd-core@latest`).

## Deferred Items

| Category | Item | Status | Deferred At |
|----------|------|--------|-------------|
| *(none)* | | | |

## Session Continuity

Last session: 2026-10-20
Stopped at: Phase 6 complete; Phase 7 not planned
Resume file: None
