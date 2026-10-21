# focker

## What This Is

focker is a Linux command-line tool that moves running processes from one parent to another, cleanly. "Think docker, but it's meet the Fockers." It runs a subreaper supervisor per "family". A cutover freezes a child, takes over the old parent's pipes and sockets to it, makes the old parent exit, verifies the adoption, and thaws the child. A handover can then exec any program in the supervisor's place, and that program inherits the children as real children. It's for people who need to replace or rescue a parent process without restarting its children.

## Core Value

A cutover must never leave a child worse off. Either the child ends up adopted, running, with its I/O intact, or everything rolls back and it stays exactly where it was.

## Requirements

### Validated

<!-- Shipped in the initial commit 3619015 and covered by tests/run.sh (71 checks). -->

- ✓ `focker run` starts a family under a `PR_SET_CHILD_SUBREAPER` supervisor, in the foreground (exit status passthrough) or detached (`-d`, log file), with `--linger`
- ✓ `focker cutover` adopts named PIDs or `--from PPID`: ptrace freeze of all threads, pipe and unix-socketpair takeover via `pidfd_getfd` with draining to logs, old parent signalled via pidfd, adoption verified, thaw
- ✓ Signals that arrive mid-cutover are swallowed under ptrace freeze (`PR_SET_PDEATHSIG` other than SIGKILL, parents that kill their children on shutdown)
- ✓ `--freeze signal|none`, `--wait`, `--timeout` with full rollback, `--dry-run` plans, bystander reporting, and grandchild generation-skip
- ✓ `focker handover` execs a new parent in place (same PID), passing `FOCKER_CHILDREN`/`FOCKER_FDS`/`FOCKER_NAME`/`FOCKER_STATE_DIR`. Failed exec recovers
- ✓ `ps [-a]`, `tree`, `inspect`, `logs [-f] [-p PID]`, `freeze`/`thaw`, `stop`, `rm`. Exit codes 0/1/2. `--help` everywhere
- ✓ Documentation: README, docs/USAGE, ARCHITECTURE, HOWTO, LIMITATIONS

- ✓ v1.0 Hardening (2026-10-20): cutover edge cases (REL-01..04), test coverage and parser unit tests (TST-01..06), sanitizer/analyzer builds and a disabled CI workflow (BLD-01..03), `task/*/children` child lookup and measured idle cost (PERF-01..02). Archive: `.planning/milestones/`

### Active

<!-- Milestone v2.0: Reach. -->

- [x] Release a single held fd, and JSON output for `ps`/`inspect` (FD-02, OUT-01)
- [x] Take over loopback TCP connections between old parent and child (FD-01)
- [ ] Run the cutover as a non-blocking state machine in the poll loop (ASYNC-01)
- [ ] Adopt processes not started under focker via a capability-granted CRIU (CRIU-01)
- [ ] Stubborn parents: warn about TERM-catching parents, `--kill-after`, `--tree` (STUB-01..03)
- [ ] `focker wait` for scripts (WAIT-01)
- [ ] An invisible, ptrace-based pause (PAUSE-01)
- [ ] Pty takeover: a held pty master, and a fresh pty for terminal-bound jobs (PTY-01, PTY-02)

### Out of Scope

- Non-Linux platforms. Never: the tool is built on Linux-only kernel interfaces
- Privileges on focker itself. CRIU-01 may only use privilege held by the `criu` binary
- Eventfds, memfds, and moving pgrp/session/cgroup. No demand yet (ptys are in scope since 2026-10-21: PTY-01/02)

## Context

- Linux kernel facts the design rests on: the parent changes only when the parent dies; orphans go to the nearest ancestor subreaper; execve keeps the PID, the children and the subreaper flag; `pidfd_getfd` copies fds; ptrace-stopped tasks don't take signals until resumed. Details are in docs/ARCHITECTURE.md §1.
- Brownfield onboarding: the codebase map is in .planning/codebase/ (mapped at 3619015). Known issues are in .planning/codebase/CONCERNS.md.
- GSD Core is installed for Gemini on this machine, not for Claude Code. These artifacts were written by hand following the gsd-core templates, and the `gsd-tools init onboard` projection confirms they are complete.

## Constraints

- **Tech stack**: C11 + glibc + Linux UAPI only, with no third-party dependencies. It's a low-level tool that must build anywhere with a compiler.
- **Compatibility**: Linux ≥ 5.6, glibc ≥ 2.32. Those are the pidfd syscalls and `sigabbrev_np`.
- **Security**: no root, no capabilities, same-uid control socket. Ptrace only on descendants (Yama scope 1).
- **Behavior**: the CLI contract (exit codes, transcript line prefixes, `FOCKER_FDS` format) is relied on by tests and scripts. Changes must be deliberate and documented in docs/USAGE.md.

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Reparent by subreaper + old-parent death | The only kernel mechanism that rewrites `real_parent` | ✓ Good |
| ptrace (SEIZE+INTERRUPT) is the default freeze | Invisible to child and parent; lets mid-cutover signals be swallowed | ✓ Good |
| Handover by execve in place | Keeps the PID, so the new program becomes the real parent | ✓ Good |
| Take over only *linked* fds (opposite pipe ends, unix peers) | Inherited duplicates don't matter, and stealing a shared read end would race the child | ✓ Good |
| Single-threaded synchronous supervisor | Simplest correct design; cutovers take milliseconds | ⚠️ Revisit in v2.0 Phase 7 |
| GSD artifacts hand-written (no Claude Code GSD install) | User chose artifacts only | ✓ Good (validated with gsd-tools through v1.0) |
| Two sanitizer builds, not one combined | UBSan ignores `log_path` when combined with ASan | ✓ Good |
| `handover` replies `exec`, then EOF or `err` | The client must learn whether execve worked | ✓ Good |
| Public repo github.com/bsommers/focker, CI disabled in `ci/` | User request | — Pending (CI) |
| CRIU-01 uses a `criu` binary the user has granted `CAP_CHECKPOINT_RESTORE` (option a) | User chose (a): focker stays unprivileged, the capability sits on criu | — Pending (Phase 8) |

---
*Last updated: 2026-10-20 after milestone v1.0, starting v2.0*
