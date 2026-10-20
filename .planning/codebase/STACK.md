---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# Technology Stack

**Analysis Date:** 2026-10-20

## Languages

**Primary:**
- C11 with GNU extensions (`-std=c11 -D_GNU_SOURCE`): all of `src/` and `examples/`

**Secondary:**
- Bash: `tests/run.sh` (integration tests) and `examples/demo.sh` (narrated walkthrough)

## Runtime

**Environment:**
- Linux 5.6 or later. Required syscalls: `pidfd_open`, `pidfd_getfd`, `pidfd_send_signal`, `PR_SET_CHILD_SUBREAPER`, `PTRACE_SEIZE`/`PTRACE_INTERRUPT`, `signalfd`, `NETLINK_SOCK_DIAG` (`unix_diag`)
- glibc 2.32 or later (`sigabbrev_np`). Developed on glibc 2.43, kernel 7.0, gcc 15.2
- pidfd syscalls are called via `syscall(SYS_*)` (`src/util.c`), so no particular glibc wrapper version is needed

**Package Manager:**
- None. No third-party dependencies.

## Frameworks

**Core:**
- None. Only libc and Linux UAPI headers (`linux/netlink.h`, `linux/rtnetlink.h`, `linux/sock_diag.h`, `linux/unix_diag.h`)

**Testing:**
- A hand-rolled bash harness, `tests/run.sh`, with `ok`/`bad`/`check`/`expect_rc`/`wait_for` helpers. It runs 71 checks against real processes.

**Build/Dev:**
- GNU Make (`Makefile`): targets `all`, `examples`, `test`, `demo`, `install`, `uninstall`, `clean`
- cc/gcc with `-Wall -Wextra -Wshadow -Wformat`. Examples link with `-pthread`.

## Key Dependencies

**Critical:**
- Linux procfs: `/proc/PID/{status,cmdline,comm,fd,fdinfo,task}` is the only source of process state (`src/procfs.c`)
- Linux ptrace: the default freeze method and signal swallowing (`src/cutover.c`)
- NETLINK_SOCK_DIAG: matching unix socket peers (`src/unixdiag.c`)

## Configuration

**Environment:**
- `FOCKER_RUNTIME_DIR`: family state root. Default `$XDG_RUNTIME_DIR/focker`, then `/tmp/focker-$UID` (`runtime_dir()` in `src/util.c`)
- `FOCKER_FREEZE`: default `cutover --freeze` method (`src/main.c`)
- Precedence is flag > env > default

**Build:**
- `Makefile` variables: `CC`, `CFLAGS`, `PREFIX` (default `/usr/local`), `DESTDIR`

## Platform Requirements

**Development:**
- Linux only. Yama `kernel.yama.ptrace_scope` must be 0 or 1 for ptrace freezing and fd takeover.
- No root needed for build or tests.

**Production:**
- A single static-ish binary, `build/focker`, installed with `make install`. It runs as the invoking user with no capabilities.

---

*Stack analysis: 2026-10-20*
