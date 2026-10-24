# Phase 8 CRIU Adoption Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `focker adopt-external NAME PID` moves a job (its whole process group, so a pipeline) started in another terminal under a focker family via criu, onto a focker-held pty whose output goes to `focker logs NAME`; `focker fg NAME` attaches the current terminal to it, one terminal at a time.

**Architecture:** The supervisor becomes able to own one pty per family: it is a session leader (detached families already `setsid`), takes the pty slave as its controlling terminal, hands the slave to the job, and drains the master into `focker.log` as a held-fd slot of kind `pty`. `focker fg` gets the master over the control socket with `SCM_RIGHTS` and copies bytes itself while the supervisor stops draining. `adopt-external` runs the pre-flight (criu capability, job discovery, trial dumps) and the prompt in the client, then sends one `adopt` request; the supervisor does the real dump (`--leave-stopped`), the kill, and `criu restore --restore-detached --shell-job` as its own child, so the restored tree is orphaned onto it (it is a subreaper). `run -d -t` (new) starts a command on that same pty, which is what lets `fg` be tested without criu.

**Tech Stack:** C11, Linux pty (`posix_openpt`, `TIOCSCTTY`, `tcsetpgrp`), `SCM_RIGHTS`, criu 4.2 run unprivileged (`--unprivileged`), `tests/run.sh`, a new Python 3 pty driver `tests/ptyrun.py`.

**Spec:** `docs/superpowers/specs/2026-10-21-criu-adoption-design.md` (read it; this plan argues from it).

Deviation from the spec, decided here: the spec says to read criu's `security.capability` xattr. This plan probes behavior instead (`criu check --unprivileged` and its `CAP_CHECKPOINT_RESTORE` message), because a wrapper script around criu (used by the restore-failure test, and by users with wrappers) has no xattr but works. Also new: `run -d -t`, so `fg` is testable without criu. Also `.planning/ROADMAP.md` Phase 8, `.planning/REQUIREMENTS.md` CRIU-01.

## Global Constraints

- focker gains no privilege and never runs `sudo`. The capability is on `criu`: `sudo setcap cap_checkpoint_restore+eip $(command -v criu)` (user decision 2026-10-20, option a). If Task 1 shows `cap_sys_ptrace` is also needed under Yama `ptrace_scope=1`, the message names both and the user is told before Task 6 (it widens their decision).
- "Without a capable criu it exits 1 with a message naming the `setcap` command, and changes nothing."
- "An integration test runs when a capable criu is present and is reported as `skip` otherwise." The skip line format is the existing `  skip  <name>`; the summary line `N passed, M failed` does not change.
- Pipelines move together: the unit is the process group of PID (user decision).
- Prompt `kill the original and restore under NAME? [y/N]` before anything is stopped; `--yes` skips; no `--yes` and stdin not a terminal → exit 1, nothing touched (user decision).
- `fg`: one terminal at a time; a second `fg` takes over and the first exits 0 printing `taken over by another terminal` (user decision). Detach key Ctrl-P Ctrl-Q. `fg` exits with the job's exit code when the job ends.
- `fg` is read-only in the Phase 7 sense (answered during an in-flight cutover); `adopt` is mutating (refused with `busy (a cutover is in progress)`).
- Exit codes 0 / 1 runtime / 2 usage; existing transcript lines and the existing control requests' reply format unchanged. No new build dependencies. `make ci` green (plain, ASan, UBSan, `-fanalyzer`, shellcheck) after every task.
- Never stage `docs/plans/checkpoint.md`; never `git add -A` or `git add docs`. Don't move `ci/github-actions.yml`.

## Review Focus

- **The client running `adopt-external` is killed (Ctrl-C in terminal B) after the request is sent:** the supervisor finishes or rolls back on its own; the job is never left stopped. (Task 6, `a vanished adopt client does not strand the job`.)
- **criu dump fails after the job was stopped** (images dir unwritable, criu error): the job is resumed in terminal A and is not a member. (Task 6, `a failed dump resumes the job where it was`.)
- **`fg` exits by any path** (detach, takeover, job exit, SIGTERM to `fg`, supervisor killed): terminal settings are restored. (Task 4, `fg restores the terminal on every exit path`, one check per path.)
- **The job is a background job whose shell in terminal A exits after the move:** the moved job gets no SIGHUP. (Task 6, `closing terminal A does not reach the moved job`.)
- **`fg` against a family whose pty job already exited:** exits with the job's status at once, does not hang. (Task 4, `fg after the job exited reports its status`.)

---

### Task 1: Spike (throwaway): criu unprivileged dump/restore onto a new pty, pipelines

**Precondition:** the user has run `sudo setcap cap_checkpoint_restore+eip /usr/sbin/criu`. If `criu check --unprivileged` still prints `needs to have the CAP_SYS_ADMIN or the CAP_CHECKPOINT_RESTORE`, stop and ask the user; do not run sudo.

**Files:**
- Create (scratchpad, not committed): spike scripts.
- Create: `.planning/phases/08-criu-adoption/08-SPIKE.md` (findings, committed).

**Interfaces:**
- Produces: `08-SPIKE.md` answering each question below with the exact command lines that worked. Tasks 5 and 6 build the criu argv from it; if an answer contradicts this plan, the executor records `Ruling:` and follows the spike.

- [ ] **Step 1:** In a pty started by `python3 -c 'import pty; pty.spawn(["bash","--norc","-i"])'`-style driver (terminal A), start `examples ticker &` and a pipeline `ticker | cat &`. Answer:
  1. Does `criu dump --unprivileged --shell-job -t PID` work with only `cap_checkpoint_restore` under `ptrace_scope=1`, or is `cap_sys_ptrace` needed? (If needed, stop and tell the user before continuing.)
  2. The `--external tty[RDEV:DEV]` / `--inherit-fd fd[N]:tty[RDEV:DEV]` spelling (hex? which st_rdev/st_dev) that restores the job onto a new pty slave passed as fd N.
  3. Restore from a process that is a session leader with the new pty as its controlling terminal, with `--restore-detached`, under a `PR_SET_CHILD_SUBREAPER` parent: does the restored root end up a child of the subreaper, in that session, with the pty as its ctty? Does `tcsetpgrp(master, job_pgid)` from the session leader then make Ctrl-C (byte 0x03 on the master) deliver SIGINT to the job?
  4. Does `criu restore` honor `--leave-stopped` (restore all roots stopped, then SIGCONT)?
  5. Pipeline: dump each root separately with the group SIGSTOPped first; restore each with `--inherit-fd fd[N]:pipe:[INODE]` using one fresh pipe. Is data that was in the pipe buffer delivered exactly once (write a known count of lines, compare)?
  6. What `criu dump` prints when it refuses (established TCP, a socket to outside the tree), so pre-flight can show the line.
- [ ] **Step 2:** Write `08-SPIKE.md` (question, answer, command). Commit: `git add .planning/phases/08-criu-adoption/08-SPIKE.md && git commit -m "planning: Phase 8 spike findings (criu unprivileged, pty, pipelines)"`.

---

### Task 2: A family can own a pty (`run -d -t`)

**Files:**
- Create: `src/pty.c` (pty allocation and controlling-terminal setup)
- Modify: `src/focker.h`, `src/supervisor.c` (`supervisor_run` gains a tty flag; first child on the pty; `pty` slot drained to `focker.log`), `src/main.c` (`run -t`, requires `-d`; `RUN_USAGE`)
- Test: `tests/run.sh` new section `pty families`

**Interfaces:**
- Produces, in `src/focker.h`:
  - `int pty_open(int *master, int *slave, const struct termios *tio, const struct winsize *ws);` both `O_CLOEXEC`, slave opened `O_NOCTTY`; `tio`/`ws` may be NULL (defaults). 0 or -1/errno.
  - `int pty_take_ctty(int slave);` `TIOCSCTTY` for the calling session leader. 0 or -1.
  - `int supervisor_run(const char *name, const char *dir, char **argv, bool detached, bool linger, bool tty, int ready_fd);` `argv == NULL` starts no first command (used by Task 6); `tty` requires `detached`.
  - In `supervisor.c`: `static int fam_pty = -1;` the slot index of the family's `pty` stolen entry, or -1. At most one per family.
  - Held-fd kind string `"pty"`, `child` = the job's first process, `child_fd` = 0, `logfd` = a separate `O_APPEND` open of `STATE_DIR/focker.log`, `log` = that path, `drain = true`.
- [ ] **Step 1: Write the failing tests** in `tests/run.sh`:
  - `run -t without -d is a usage error` (`expect_rc ... 2 "$F" run -t -n x true`).
  - `a tty family's command sees a terminal`: `"$F" run -d -t -n tty1 sh -c 'tty; stty size; sleep 30'`; `wait_for grep -q '^/dev/pts/' $FOCKER_RUNTIME_DIR/tty1/focker.log`.
  - `inspect lists the pty`: `grep -q 'pty' <<<"$("$F" inspect tty1)"`.
  - `the command is the terminal's foreground group`: in the log, `ps -o tpgid=,pgid= -p CHILD` equal.
  - `a tty family exits when its command does`: `run -d -t -n tty2 sh -c 'exit 4'` → `exit` file says `exited 4`.
- [ ] **Step 2:** `make && tests/run.sh` → the new checks FAIL (unknown option `-t`).
- [ ] **Step 3: Implement.** In `supervisor_run` with `tty`: `pty_open`, `pty_take_ctty(slave)` (the detached supervisor already leads its session), fork; child: `setpgid(0,0)`, dup2 slave to 0-2, exec; parent: `tcsetpgrp(slave, child)` (ignore SIGTTOU around it), close slave, register the `pty` slot. The supervisor keeps the master only. `run -t` without `-d` → usage error `-t needs -d (a foreground family already has your terminal)`.
- [ ] **Step 4:** `make ci` → all PASS (new checks ok).
- [ ] **Step 5: Commit** `git add src/pty.c src/focker.h src/supervisor.c src/main.c tests/run.sh && git commit -m "run: -t starts a detached family's command on a focker-held pty"`

---

### Task 3: `tests/ptyrun.py`, the terminal driver for tests

**Files:**
- Create: `tests/ptyrun.py`
- Test: `tests/run.sh` (one self-check)

**Interfaces:**
- Produces: `python3 tests/ptyrun.py [--rows R --cols C] [--send TEXT@SECONDS]... [--stty-check] [--timeout S] -- CMD...`. Runs CMD on a fresh pty, writes each `--send` (Python escapes: `\x03`, `\x10\x11`) after the given delay, copies everything CMD prints to stdout, exits with CMD's exit code (128+sig if killed). `--stty-check` records `termios` of the pty before and after CMD and exits 99 if they differ. `--timeout` (default 10) kills CMD and exits 124.
- [ ] **Step 1:** Test `ptyrun runs a command on a terminal`: `python3 tests/ptyrun.py -- sh -c 'tty; exit 3'` → output matches `^/dev/pts/`, rc 3. Run → FAIL (no file).
- [ ] **Step 2:** Write `ptyrun.py` (stdlib only: `pty`, `os`, `select`, `termios`, `fcntl`, `struct`). Run → PASS. shellcheck/`make lint` unaffected.
- [ ] **Step 3: Commit** `git add tests/ptyrun.py tests/run.sh && git commit -m "tests: ptyrun.py drives commands on a pty"`

---

### Task 4: `focker fg NAME`

**Files:**
- Create: `src/attach.c` (the client side: raw mode, copy loop, detach key, winsize, exit paths)
- Modify: `src/supervisor.c` (request `attach`; attached client fd in the poll set; pause/resume draining; notifications), `src/client.c` (receive an fd with `SCM_RIGHTS`), `src/main.c` (command `fg`, `FG_USAGE`), `src/focker.h`
- Test: `tests/run.sh` section `fg`

**Interfaces:**
- Consumes: Task 2's `fam_pty`, the `pty` slot; Task 3's `ptyrun.py`.
- Produces:
  - Control request `attach`. Reply on success: `ok\n` with the pty master passed as `SCM_RIGHTS` ancillary data on the reply's first byte; the connection stays open. Failure: `err\n<NAME> has no terminal\n` (exit 1 in the client).
  - Later lines the supervisor writes on that connection: `taken-over\n` (a newer `attach` replaced it; the supervisor then closes it) or `exit <CODE> <describe_status text>\n` when the pty job's last process is reaped (CODE as `supervisor_run` computes from a status: `WEXITSTATUS` or 128+sig).
  - The job for `fg`'s exit status: the pty slot's processes; the reported status is that of the last of them reaped.
  - `int client_attach(const char *name, int *master, int *conn);` in `client.c`: 0, 1 (err reply printed), -1 unreachable.
  - `int cmd_fg(const char *name);` in `attach.c`.
  - While a client is attached the supervisor does not drain the master; when the connection closes (detach, crash) draining resumes. `attach` is not in the busy list.
- [ ] **Step 1: Write the failing tests** (all through `ptyrun.py` on a `run -d -t` family running `sh -c 'trap "echo got-INT" INT; while read l; do echo "echo:$l"; done; exit 7'`):
  - `fg shows the job's output and passes input`: send `hi\n` → output contains `echo:hi`.
  - `Ctrl-C reaches the job`: send `\x03` → `got-INT`.
  - `the job sees fg's window size`: family running `sh -c 'while sleep 0.2; do stty size; done'`, ptyrun `--rows 33 --cols 77` → `33 77` appears.
  - `Ctrl-P Ctrl-Q detaches, exit 0`, and `output goes back to the log after detach` (a line echoed after detach appears in `focker.log`).
  - `Ctrl-P then another key is passed through` (send `\x10a\n` → `echo:` line contains byte 0x10 then `a`; check with `grep -a`).
  - `a second fg takes over`: first ptyrun in background, second started; first exits 0 and printed `taken over by another terminal`; second still receives `echo:` lines.
  - `fg exits with the job's exit code`: send `\x04` (EOF) → job exits 7 → ptyrun rc 7.
  - `fg after the job exited reports its status` (family with `--linger`): rc 7 at once.
  - `fg on a family without a terminal exits 1` with `has no terminal`.
  - `fg restores the terminal on every exit path`: `--stty-check` on detach, takeover, job exit, `kill -TERM` of fg, `kill -KILL` of the supervisor → none returns 99.
  - `fg is answered during a cutover`: a `run -d -t` family whose command is `oldparent` with ticker children; start a cutover that waits (`-s 0 --timeout 5`) in the background; while it is in flight, `fg` attaches and shows output (the busy flag is per supervisor, so it must be the same family).
- [ ] **Step 2:** Run → FAIL (`unknown command 'fg'`).
- [ ] **Step 3: Implement.** Supervisor: `attach` handler sends the fd with `sendmsg`, keeps `attached_cfd`, sets the slot's `drain = false`; a second `attach` writes `taken-over\n` to and closes the old one; `attached_cfd` is polled for `POLLHUP|POLLIN` (any input or hangup = detach → `drain = true`); the reaper sends the `exit` line when the last pty-job process is reaped (the slot's master then reads EIO; close the slot as `drain()` does). Client (`cmd_fg`): require stdin a terminal (else exit 1 `fg needs a terminal`); save termios, `cfmakeraw`, `atexit` + SIGTERM/SIGINT/SIGHUP handlers restore it; set the master's winsize from stdin's and again on SIGWINCH (signalfd or self-pipe); poll stdin, master, conn; detach state machine for 0x10 0x11; conn line `taken-over` → restore, print `taken over by another terminal`, exit 0; `exit CODE ...` → restore, print `NAME: <text>`, exit CODE; conn EOF without a line → restore, print `supervisor went away`, exit 1.
- [ ] **Step 4:** `make ci` → PASS.
- [ ] **Step 5: Commit** `git add src/attach.c src/supervisor.c src/client.c src/main.c src/focker.h tests/run.sh && git commit -m "fg: attach a terminal to a family's pty; detach, takeover, exit status"`

---

### Task 5: `adopt-external` pre-flight, dry run and prompt (client side)

**Files:**
- Create: `src/criu.c` (finding criu, the capability probe, job discovery, argv builders, trial dump)
- Modify: `src/procfs.c` (`proc_pgid`, `proc_tty_nr`, `proc_uid`), `src/main.c` (command `adopt-external`, `ADOPT_USAGE`), `src/focker.h`, `tests/unit.c`
- Test: `tests/unit.c`, `tests/run.sh` section `adopt-external` (criu-gated parts use `skip`)

**Interfaces:**
- Consumes: `08-SPIKE.md` for the exact criu options.
- Produces, in `src/focker.h`:
  - `pid_t proc_pgid(pid_t pid);` `int proc_tty_nr(pid_t pid);` (field 7 of `/proc/PID/stat`, 0 = none) `uid_t proc_uid(pid_t pid);` (`(uid_t)-1` if gone)
  - `#define MAX_ROOTS 16`
  - `struct criu_job { pid_t pgid; pid_t roots[MAX_ROOTS]; int nroots; pid_t members[MAX_PIDS]; int nmembers; bool tty; unsigned long tty_rdev, tty_dev; unsigned long pipes[MAX_ROOTS]; int npipes; };` `pipes` = inodes of pipes connecting two roots.
  - `int job_roots(const struct procent *ps, int n, const pid_t *pgids, pid_t pgid, pid_t *roots, int max);` pure: roots = members of `pgid` whose parent is not a member, sorted by pid. Unit-tested.
  - `int criu_find(char *path, size_t len);` `$FOCKER_CRIU`, else `criu` on `PATH`. 0 or -1.
  - `int criu_capable(const char *criu, char *why, size_t len);` runs `criu check --unprivileged`; not capable when it fails to run or its output contains `CAP_CHECKPOINT_RESTORE` (the text seen on this machine: `CRIU needs to have the CAP_SYS_ADMIN or the CAP_CHECKPOINT_RESTORE capability`). 1 capable, 0 not.
  - `int criu_job_load(pid_t pid, struct criu_job *j, char *why, size_t len);` 0 or -1 with `why`.
  - `int criu_dump_argv(const char *criu, const struct criu_job *j, pid_t root, const char *dir, bool trial, char **argv, int max);` and `int criu_restore_argv(const char *criu, const struct criu_job *j, pid_t root, const char *dir, int tty_fd, const int *pipe_fds, char **argv, int max);` return argc; strings in a static or caller arena (executor's choice, document it). Unit-tested for exact flags: dump always has `dump --unprivileged --shell-job -t ROOT -D DIR`, `--leave-running` when `trial`, else `--leave-stopped`; the tty and pipe options per the spike. Restore always has `restore --unprivileged --shell-job --restore-detached -D DIR`, plus `--inherit-fd` per the spike.
  - `int cmd_adopt_external(const char *name, pid_t pid, bool yes, bool dry_run);` in `criu.c`.
  - Messages (exact): no criu: `focker: criu not found (install it, or set FOCKER_CRIU)`; not capable: `focker: criu at PATH lacks CAP_CHECKPOINT_RESTORE; grant it once with:\n  sudo setcap cap_checkpoint_restore+eip PATH` (plus `cap_sys_ptrace` if the spike requires it); refused trial: `focker: criu cannot move PID: <criu's error line>; nothing was changed`.
- Pre-flight order (stop at first failure, exit 1, nothing touched): find criu → capable → PID exists, same uid, not the supervisor of or a member of any running family (check every family's `pid` file and its `members` reply) → `criu_job_load` (roots ≤ 16, every member same uid) → trial dump of each root into `mkdtemp(RUNTIME/.adopt-XXXXXX)`, deleted after. `--dry-run` prints `plan: job PGID: roots ...`, `plan: terminal /dev/pts/N` (or `no terminal`), `plan: family NAME (new|existing)` and exits 0. Then the prompt (Global Constraints). Then Task 6's request.
- [ ] **Step 1: Unit tests (failing)** in `tests/unit.c`: `job_roots` for a single process, a 3-process pipeline under a shell (roots are the 3 siblings), a root with children in the same group (1 root), members out of pid order (sorted result); `criu_dump_argv` trial vs real; `criu_restore_argv` with and without tty and with 1 pipe.
- [ ] **Step 2: Integration tests (failing)** in `tests/run.sh`, always run: `adopt-external without a capable criu exits 1 and names setcap` (`FOCKER_CRIU=/usr/sbin/criu` when not capable; if this machine's criu IS capable, point `FOCKER_CRIU` at a copy in the test dir, which has no file caps) and the job is still a child of its parent and running; `missing PID is a usage error` (rc 2); `FOCKER_CRIU=/nonexistent` → `criu not found`. Criu-gated (helper `criu_ok` = capable; else `skip`): `refuses a focker member`, `refuses a job with an established TCP connection, nothing changed`, `--dry-run plans a pipeline's roots`, `no --yes and stdin not a terminal exits 1, nothing changed`, `answering n exits 1, nothing changed` (via `ptyrun.py --send 'n\n@1'`).
- [ ] **Step 3:** Run → FAIL. **Step 4:** Implement. **Step 5:** `make ci` → PASS (gated checks print `skip` if criu is not capable).
- [ ] **Step 6: Commit** `git add src/criu.c src/procfs.c src/main.c src/focker.h tests/unit.c tests/run.sh && git commit -m "adopt-external: pre-flight, trial dump, dry run and confirmation"`

---

### Task 6: The move (supervisor side), pipelines, failure paths

**Files:**
- Create: `src/adopt.c` (the `adopt` request: stop, dump, kill, restore, verify)
- Modify: `src/supervisor.c` (request `adopt`, busy list, supervisor start with no command), `src/main.c` (start-or-join a detached family; factor `cmd_run`'s detach branch into `static int start_detached(const char *name, const char *dir, char **cmd, bool linger, bool tty)`), `src/criu.c` (send the request), `src/focker.h`
- Test: `tests/run.sh` section `adopt-external` (criu-gated)

**Interfaces:**
- Consumes: Task 2 (`pty_open`, `pty_take_ctty`, `pty` slot, `supervisor_run(..., argv=NULL, ...)`), Task 5 (`struct criu_job`, the argv builders).
- Produces:
  - Control request `adopt criu=PATH pgid=P root=R... pipe=INODE... tty=RDEV:DEV|none` (key=value tokens, like `cutover`). Reply: transcript lines `adopt: ...` then `ok`/`err`.
  - `int adopt_run(struct family *f, const struct criu_job *j, const char *criu, struct sbuf *out);` in `adopt.c`; returns 0/1.
  - Joining: a running family is joined only if its supervisor is detached (it leads its own session) and `fam_pty == -1` when the job has a terminal; else exit 1 `family NAME cannot take a terminal job (it already has one, or runs in the foreground)`. A new family is `start_detached(name, dir, NULL, true, false)`.
  - Images: `STATE_DIR/criu/ROOT/`; deleted on success; kept on failure with the line `adopt: restore failed; images kept in DIR; retry with: <criu restore argv>`.
- Sequence in `adopt_run` (each step logs one `adopt:` transcript line): (1) `kill(-pgid, SIGSTOP)` and wait until every member is `T`; (2) if tty: `pty_open` with the original terminal's termios/winsize (read via `/proc/ROOT/fd/0..2` → open the tty `O_RDONLY|O_NOCTTY|O_NONBLOCK`, `tcgetattr`, `TIOCGWINSZ`), `pty_take_ctty`; (3) dump each root (`--leave-stopped`) — any failure: `kill(-pgid, SIGCONT)`, close the pty, exit 1 `adopt: dump failed: <line>; PID continues where it was`; (4) point of no return: `kill(-pgid, SIGKILL)`, wait until the PIDs are gone (pidfds); (5) restore each root as a child of the supervisor with the slave (and pipe ends) at the fds the spike chose; (6) verify every member PID is alive with the supervisor as an ancestor and parent of each root, record members (origin `'x'`, shown as `adopted from another terminal`), `tcsetpgrp` to `pgid`, `kill(-pgid, SIGCONT)` if the spike's restore leaves them stopped; register the `pty` slot; delete images. Any failure in (5)/(6): kill what was restored, keep images, print the retry line, exit 1.
  - criu runs synchronously in the request handler with a 60 s cap per invocation (`Ruling` in the ledger: bounded, user-initiated; held fds pause for its duration).
  - The reaper must not reap criu's own child processes out from under the `waitpid` in `adopt_run`: wait for criu by pid before returning to the loop (`waitpid(criu_pid, ..., 0)` is safe because `reap_children` only runs from the loop).
- [ ] **Step 1: Failing tests** (criu-gated; terminal A = `python3 tests/ptyrun.py --timeout 60 -- bash --norc -i` in the background, fed `ticker -i 0.2 &` / a pipeline via `--send`; PIDs read from a file the job writes):
  - `a background job moves: same PID, member, parent is the supervisor`.
  - `its output goes to focker logs` (`ppid=$SUP` lines in `focker.log`).
  - `closing terminal A does not reach the moved job` (kill the ptyrun; job alive 1 s later).
  - `fg shows the moved job and Ctrl-C reaches it`.
  - `a pipeline moves together and its pipe still carries data exactly once` (`seq 1 100000 | slow-cat` style using fixtures; count lines at the end = 100000).
  - `a job with no terminal moves; fg says it has no terminal`.
  - `joining a family that already has a terminal job is refused, nothing changed`.
  - `a failed dump resumes the job where it was` (make `STATE_DIR/criu` a file so the dump cannot write; job still a child of terminal A's shell and not stopped).
  - `a failed restore keeps the images and prints a retry line` (`FOCKER_CRIU` = a wrapper script that execs the real criu for everything but `restore`, where it exits 1; the capability probe runs through the wrapper, so it still passes).
  - `a vanished adopt client does not strand the job` (kill the client right after it sends the request; job ends up a running member).
  - `adopt is refused with busy during a cutover`.
- [ ] **Step 2:** Run → FAIL (or `skip` when criu is not capable: then run them once with the capability before the commit, and say so in the ledger).
- [ ] **Step 3:** Implement. **Step 4:** `make ci` → PASS.
- [ ] **Step 5: Commit** `git add src/adopt.c src/supervisor.c src/main.c src/criu.c src/focker.h tests/run.sh && git commit -m "adopt-external: move a job from another terminal under a family via criu"`

---

### Task 7: Docs and planning

**Files:**
- Modify: `docs/USAGE.md` (`run -t`, `fg`, `adopt-external`, requests `attach` and `adopt`, the `inspect` `pty` kind and JSON), `docs/HOWTO.md` (the CRIU section becomes the command; the manual recipe stays as background), `docs/LIMITATIONS.md` ("Only descendants" → adopt-external, its own limits: established TCP, one terminal job per family, the setcap precondition), `docs/USE-CASES.md` (walkthrough: start in terminal A, pick up in B), `docs/ARCHITECTURE.md` (pty ownership, attach protocol, the move sequence), `README.md` (command list), `CHANGELOG.md` (Unreleased Added), `.planning/REQUIREMENTS.md` (CRIU-01 complete), `.planning/ROADMAP.md` (Phase 8 done; Phase 12 note: `fg` exists), `.planning/STATE.md`, `.planning/phases/08-criu-adoption/08-01-SUMMARY.md`
- Test: `make ci`; every command example in the docs run by hand once.
- [ ] **Step 1:** Write the docs. **Step 2:** `make ci` → PASS. **Step 3: Commit** with explicit paths (never `git add docs`): `git commit -m "docs: adopt-external, fg, run -t; Phase 8 complete"`
