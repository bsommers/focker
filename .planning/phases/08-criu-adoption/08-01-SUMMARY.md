---
phase: 08-criu-adoption
plan: 01
subsystem: supervisor, client
tags: [criu, pty, adopt-external, fg, pipelines]
requires: [07-01]
provides: [run -d -t, focker fg, focker adopt-external, attach and adopt requests, origin external, held-fd kind pty]
affects: [12-pty-takeover]
key-files:
  created: [src/pty.c, src/attach.c, src/criu.c, src/adopt.c, tests/ptyrun.py, tests/mkjob.py]
  modified: [src/supervisor.c, src/client.c, src/main.c, src/procfs.c, src/focker.h, tests/run.sh, tests/unit.c, docs/USAGE.md, docs/HOWTO.md, docs/LIMITATIONS.md, docs/USE-CASES.md, docs/ARCHITECTURE.md, README.md, CHANGELOG.md]
key-decisions:
  - "the capability goes on criu (CAP_CHECKPOINT_RESTORE); focker stays unprivileged and never runs sudo"
  - "under Yama ptrace_scope 1 the user runs the sysctl; focker detects criu's refusal and names it (user: 'then, do option 1')"
  - "a job is PID's process group plus every root's subtree; a pipeline moves together (user: '1 - pipelines move together')"
  - "fg: one terminal at a time, a second fg takes it over (user)"
  - "confirmation before the move, --yes skips it (user)"
  - "the restored job gets a new process group, so the old terminal's shell cannot SIGHUP it"
requirements-completed: [CRIU-01]
completed: 2026-10-22
---

# Phase 8 Plan 01: CRIU adoption Summary

**A job started in another terminal moves under a family with `focker adopt-external NAME PID`: same PIDs, the supervisor as parent, a terminal focker holds, its output in `focker logs NAME` until `focker fg NAME`. Steps are in docs/superpowers/plans/2026-10-21-criu-adoption.md; the spike that chose the criu options is 08-SPIKE.md.**

## Accomplishments
- `run -d -t`: the detached supervisor leads its session, takes a pty as its controlling terminal, and runs the first command as the terminal's foreground group. The master drains into `focker.log` (held-fd kind `pty`). The supervisor ignores the kernel's SIGHUP when its own pty hangs up.
- `focker fg NAME`: `attach` hands over the master by `SCM_RIGHTS`; raw mode, both directions, `SIGWINCH`, Ctrl-P Ctrl-Q detach, takeover by a second `fg`, the job's exit status when it ends. The terminal is restored on every way out.
- `adopt-external`, client side: criu found and probed for the capability; the job worked out from `/proc`; refusals (another user, a session leader, focker's own job, a pipe leading out of the job, more than 64 processes or 16 roots or pipes); a trial dump of each root while the job runs (its first criu error, or the Yama sysctl); the plan; `--dry-run`; the prompt.
- The move, in the supervisor (`src/adopt.c`): stop, dump each root `--leave-stopped`, open the new pty, kill (the point of no return), new pipes between roots refilled from `pipes-data.img`, a new process group, restore each root with stdio on the slave and pipe ends by `--inherit-fd`, check, record as `external`, foreground, `SIGCONT`, delete the images. A failed dump resumes the job where it was; a failed restore keeps the images and prints the criu retry command.
- Tests: `tests/ptyrun.py` (a command on a pty) and `tests/mkjob.py` (shell-style jobs that allow any tracer, so the criu checks run under Yama scope 1). Integration checks for `run -t`, `fg`, every refusal, the dry run and prompt, a background job moved out of an interactive bash, a pipeline moved mid-stream with every line arriving exactly once, the join refusal, a job without a terminal, a failed dump, a failed restore, a vanished client, and `busy` during a cutover.

## Deviations / found along the way
- criu does not refill an inherited pipe's buffered bytes (spike); focker parses `pipes-data.img` and refills them itself.
- Terminal A's bash keeps a killed job in its table until it next reports, and SIGHUPs every listed group when its terminal closes. The first green run lost the moved job that way. The restore now puts the job in a new group led by a short-lived child.
- `start_detached` moved from main.c to client.c, since criu.c starts families and the unit binary links everything but main.o.
- The termios of terminal A are read only when the job is its foreground group, judged by `/proc/PID/stat` tpgid (`tcgetpgrp` only works on one's own terminal).
- `criu check --unprivileged` fails on unrelated host limits (no inotify instances left), so the capability probe looks only for the capability message; the trial dump tests the rest.

## Verification
- `make ci` exit 0 at d5e17ab: 108 unit + 262 integration checks under the plain, ASan and UBSan builds, no sanitizer reports.
