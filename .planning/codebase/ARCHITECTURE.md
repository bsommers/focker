---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# Architecture

**Analysis Date:** 2026-10-20

## Pattern Overview

**Overall:** a CLI client plus a long-lived per-family supervisor daemon (docker-like), talking over a unix control socket. The supervisor is a `PR_SET_CHILD_SUBREAPER`, so the kernel reparents orphans in its subtree to it.

**Key Characteristics:**
- Single-threaded, poll-driven supervisor: signalfd, listening socket, and drained fds.
- Requests are served synchronously, one at a time. A cutover blocks the loop for its duration (milliseconds, at most `--timeout`).
- No heap-heavy state. Fixed-size tables in `struct family` (`MAX_MEMBERS` 256, `MAX_STOLEN` 128).
- The kernel is the source of truth. Process state is always re-read from `/proc`, never cached.

## Layers

**CLI (`src/main.c`):** argument parsing (`getopt_long` with `+`), validation, help text, exit codes 0/1/2. Implements `run`, including detaching via fork, setsid and a ready pipe. Depends on client.c and supervisor.c.

**Client (`src/client.c`):** `client_request()` speaks the protocol. Read-mostly commands (`ps`, `tree`, `logs`, `rm`, `freeze`/`thaw`) work from `/proc` and the state dir.

**Supervisor (`src/supervisor.c`):** subreaper main loop, member table (`family_member`), reaper, drains, request dispatch (`info`, `members`, `inspect`, `cutover`, `stop`, `handover`), and handover via execve with a recovery path.

**Cutover engine (`src/cutover.c`):** the state machine validate → plan → freeze → take fds → cut ties → wait for reparent → verify → thaw/swallow → report, with rollback on failure.

**Platform readers (`src/procfs.c`, `src/unixdiag.c`):** stateless `/proc` and netlink readers that tolerate processes vanishing.

**Shared (`src/util.c`, `src/focker.h`):** `struct sbuf` string builder, logging, runtime dir and family dir, signal name parsing, pidfd wrappers, `control_addr`.

## Data Flow

**cutover:**
1. CLI validates flags and encodes them as `key=value` tokens (`cmd_cutover` in main.c).
2. The supervisor's `do_cutover` parses the tokens and calls `cutover()`.
3. `cutover()` checks that targets are descendants (`proc_is_descendant`) and builds the fd plan (`plan_fds`: pipes with opposite access mode, unix peers via unixdiag).
4. Freeze (`freeze_ptrace`: seize and interrupt every thread, re-listing until stable), then `pidfd_getfd` each planned fd into `fam.stolen[]` and `pidfd_send_signal` the old parent.
5. Poll the old-parent pidfds and re-read `PPid:` until the targets have moved (or time out and roll back).
6. Verify each target. Thaw, swallowing `pending_now & ~pending_before & ~BENIGN` via signal-delivery-stops.
7. The transcript accumulates in an `sbuf`, which goes back to the client as the reply body.

**handover:** resolve CMD → build the `FOCKER_*` env → reply → close the socket and signalfd → clear CLOEXEC on held fds → restore the signal mask → `execve`. On failure, rebuild everything and carry on.

**State Management:** in-memory `static struct family fam` in supervisor.c. On-disk files are written for `ps -a` and post-mortem only.

## Key Abstractions

- `struct family` / `struct member` / `struct stolen` (`src/focker.h`): family state; member origin `r` (run), `a` (adopted), `o` (orphan); held fds with kind `pipe-r`, `pipe-w` or `unix`.
- `struct frozen` (`src/cutover.c`): per-target thread list, signals to re-inject, pending-signal snapshot.
- `struct cutover_opts`: freeze method, signal, timeout, from, steal, drain, suppress, dry_run.

## Entry Points

- `main()` in `src/main.c`: every CLI command.
- `supervisor_run()` in `src/supervisor.c`: the daemon side (called by `run`).

## Error Handling

- Library-style functions return -1 and set errno. Commands print a one-line `focker: ...` diagnostic to stderr and return `FK_FAIL` (1). Usage errors print usage and return `FK_USAGE` (2).
- Supervisor-side failures are reported in the reply (`err` status and transcript) and logged via `logmsg`.
- Cutover failures roll back: close taken fds, thaw, leave processes with their original parents.

## Cross-Cutting Concerns

- **Signals:** ignore SIGPIPE in the supervisor and restore SIG_DFL in children and before handover. Forward `kill(1)` signals but not tty ones (`SI_KERNEL`).
- **Process groups:** in detached mode the first command gets `setpgid(0,0)` to avoid orphaned-pgrp SIGHUP/SIGCONT.
- **Security:** same-uid only. Family names are restricted to `[A-Za-z0-9_.-]`.

---

*Architecture analysis: 2026-10-20*
