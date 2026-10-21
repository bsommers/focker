---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# External Integrations

**Analysis Date:** 2026-10-20

## APIs & External Services

None. focker makes no network calls and talks to no external services.

## Kernel Interfaces (the real integration surface)

| interface | used in | purpose |
|---|---|---|
| `prctl(PR_SET_CHILD_SUBREAPER)` | `src/supervisor.c` `supervisor_run` | become the reparent target for orphans in the subtree |
| `ptrace(PTRACE_SEIZE/INTERRUPT/CONT/DETACH)` + `waitpid(__WALL)` | `src/cutover.c` `freeze_ptrace`, `thaw_ptrace` | invisible freeze; swallow signals that arrive mid-cutover |
| `pidfd_open`, `pidfd_getfd`, `pidfd_send_signal` | `src/util.c` wrappers, `src/cutover.c` | race-free signalling of the old parent; copy its pipe/socket fds |
| `NETLINK_SOCK_DIAG` / `UNIX_DIAG_PEER` | `src/unixdiag.c` | map unix socket inode to peer inode (socketpair ends have different inodes) |
| `signalfd` | `src/supervisor.c` | SIGCHLD/TERM/INT/HUP/QUIT in the poll loop |
| `waitid(WNOWAIT)` | `src/supervisor.c` `reap_children` | peek a zombie's comm before reaping |
| `execve` | `src/supervisor.c` `do_handover` | replace the supervisor in place, keeping its PID and children |
| procfs | `src/procfs.c` | ppid, state, cmdline, children, threads, fds plus access mode, pending signals |

## Data Storage

**Files:** per family, in `$FOCKER_RUNTIME_DIR/NAME/`: `control.sock`, `pid`, `cmd`, `exit`, `focker.log` (detached mode), `adopted-PID-fdN.log`.
**Databases:** none. **Caching:** none.

## IPC

- Control socket: unix `SOCK_STREAM` with NUL-separated request tokens and an `ok`/`err` first line in the reply (documented in `docs/USAGE.md` "Control protocol"). Access is restricted by dir mode 0700, umask 0177 and an `SO_PEERCRED` uid check.
- Long paths (over 108 bytes) go through `/proc/self/fd/N/control.sock` (`control_addr()` in `src/util.c`).
- Handover contract: `FOCKER_CHILDREN`, `FOCKER_FDS` (`FD:CHILD:KIND:CHILDFD`), `FOCKER_NAME`, `FOCKER_STATE_DIR` env vars.

## Authentication & Identity

Same uid only (`SO_PEERCRED`). No privilege elevation.

## Monitoring & Observability

stderr log lines prefixed `focker[NAME]:` (`logmsg()` in `src/util.c`). They go to `focker.log` when detached.

## CI/CD & Deployment

CI is written but disabled: `ci/github-actions.yml` runs `make lint`, `make test` and `make asan`, and GitHub ignores it until it is moved into `.github/workflows/`. `make ci` runs the same steps locally. Deployment is `make install`.

## Optional external tool

- CRIU is documented in `docs/HOWTO.md` as the alternative for processes not started under focker. focker does not call it.

---

*Integration audit: 2026-10-20*
