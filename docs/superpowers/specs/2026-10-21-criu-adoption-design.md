# Phase 8 design: adopting a job from another terminal (CRIU)

Status: draft for approval. Roadmap Phase 8, requirement CRIU-01.

## Intent

The user starts a job in terminal A, in the foreground or backgrounded with `&`/`bg`. Later, in terminal B, they
want that job: managed by focker, and visible in B. Today focker cannot help, because the kernel only reparents
orphans to an *ancestor* subreaper and terminal A's shell is not under focker (docs/LIMITATIONS.md, "Only
descendants").

In the user's words: "I've started something else in a different terminal, want to pick it up under a different
process. Also, if I backgrounded it, I can pick it up from a different terminal." And: "if I pick up in terminal B,
I should see output in terminal B. It could go to focker's logs until the fg." On attaching: "one at a time, a
second fg takes it over." On risk: "mitigate the risks as much as possible."

## Goals

1. `focker adopt-external NAME PID` moves the tree rooted at PID under a focker family, via a CRIU dump and
   restore. The restored processes keep their PIDs and become family members (`ps`, `tree`, `inspect`, `stop`,
   `handover` work on them as on any member).
2. A job whose stdio was a terminal is restored onto a fresh pty whose master the supervisor holds. Until someone
   attaches, the supervisor drains the master to the family log (`focker logs NAME`). Closing terminal A no longer
   reaches the job.
3. `focker fg NAME` attaches the current terminal to that pty: live output, keystrokes and Ctrl-C reach the job, the
   window size follows terminal B. A detach key returns the job to logging. One terminal at a time: a second
   `fg` takes the job over, and the first exits saying so.
4. Risk is front-loaded: everything that can be checked is checked before the job is touched, and the job is never
   silently lost.
5. Without a capable `criu`, `adopt-external` exits 1 naming the `setcap` command and changes nothing.

## Non-goals

- focker gaining any privilege. The capability belongs to `criu` (user decision 2026-10-20, option a:
  `sudo setcap cap_checkpoint_restore+eip $(command -v criu)`). focker never runs `sudo`.
- Moving jobs across machines, containers, or PID namespaces; live migration.
- Established TCP connections (criu needs CAP_NET_ADMIN for `--tcp-established`): refused, not attempted.
- Several terminals viewing one job at once (tmux-style sharing).
- The cutover-side pty work of Phase 12 (taking a pty master held by an old parent; `cutover --new-tty` by syscall
  injection). Phase 12 stays, and reuses `focker fg`.

## Anti-goals (a failure even if it "works")

- A job killed by a failed adoption with no way back. The worst allowed outcome after the point of no return is
  "restore failed; images kept at DIR; retry with: ...".
- A job restored but not reaped by focker (lost exit status), or attached to terminal A's pty still (so closing A
  hangs it up).
- `fg` that leaves terminal B in raw mode after any exit path (detach, takeover, job exit, focker crash, signal).
- The trial check disturbing the job in a way the user notices beyond a brief pause.

## How it works (decided)

### Pre-flight (nothing is touched)

In order, stopping at the first failure with exit 1 and a reason:

1. Find `criu` (`$FOCKER_CRIU`, else `PATH`) and confirm it has `cap_checkpoint_restore` in its permitted set
   (read the `security.capability` xattr; no `getcap` dependency). Missing: print the `setcap` command.
2. PID exists, is owned by the caller's uid, is not focker's own supervisor or one of its members.
3. `criu check` passes.
4. Trial dump: `criu dump --leave-running --unprivileged --shell-job --external tty[rdev:dev] -t PID` into a
   scratch directory, which is then deleted. Whatever criu refuses (sockets to outside the tree, devices,
   established TCP, pipes whose other end is outside the tree) is reported here, with criu's log line, while the
   job keeps running in terminal A. The job pauses only for the dump's duration.

`--dry-run` stops after step 4 and prints what would happen (tree, terminal found, where images go).

### The move

5. Create or join the family. If NAME has a running supervisor, the job joins it; otherwise a detached lingering
   supervisor is started for NAME, as `run -d --linger` would.
6. The supervisor allocates the pty (`posix_openpt`), copies the original terminal's termios and window size onto
   it, and holds the master.
7. Real dump with `--leave-stopped` into `STATE_DIR/criu/` (images plus criu's logs). **The job stays alive and
   stopped.** If anything fails before step 8, the job is resumed (SIGCONT) and stays in terminal A untouched.
8. Point of no return: the stopped originals are killed (their PIDs must be free for the restore).
9. The supervisor runs `criu restore --restore-detached --shell-job --inherit-fd fd[N]:tty[rdev:dev]`, with the pty
   slave at fd N. Detached, the restored tree is orphaned onto the supervisor (it is a subreaper), so it becomes a
   member and is reaped by the existing code.
10. Verify: the root PID is alive and its parent is the supervisor (same check style as cutover verify). Record the
    members. On success the image directory is deleted; on failure it is kept and the command prints the exact
    `criu restore` invocation to retry by hand.

Terminal A's shell sees its job die (it reports "Killed"). That is expected and documented.

### Jobs without a terminal

If none of the tree's fds are a terminal, steps 6 and the `--external/--inherit-fd` options are skipped; output
keeps going wherever it went (files). `fg` on such a family exits 1: "NAME has no terminal".

### `focker fg NAME`

- The client asks the supervisor over the control socket for the pty master; the supervisor passes it with
  `SCM_RIGHTS` and stops draining it to the log while a client is attached.
- The client puts its terminal in raw mode, copies both ways, forwards SIGWINCH as `TIOCSWINSZ` on the master, and
  restores the terminal on every exit path.
- Detach key: Ctrl-P Ctrl-Q (Docker's), not Ctrl-Z, which belongs to the job.
- Takeover: a second `fg` makes the supervisor drop the first client; the first `fg` restores its terminal and exits
  0 with "taken over by another terminal".
- When the job exits, `fg` prints its status and exits with the job's exit code.
- `fg` is a read-only request in the Phase 7 sense: allowed while a cutover is in flight.

### Testing

- Integration tests run only when a capable criu is found; otherwise each is reported as `skip` (Phase 8 success
  criterion 3). The test creates "terminal A" with a pty fixture (Python `pty` or `script`), starts a ticker in
  it, adopts it, checks membership, logs, PIDs unchanged, and that hanging up terminal A no longer reaches it.
- `fg` is tested through a second pty: output seen, input echoed by the job, detach restores termios, takeover ends
  the first client.
- Failure paths: no capability (exit 1, job untouched), trial dump refused (job untouched), restore made to fail
  (job not lost: images kept, retry line printed).
- Unit tests for the capability xattr parsing and the criu argv building.

### Docs

USAGE (both commands, protocol request for the pty fd), HOWTO (replace the manual criu section with the command,
keep the manual recipe as background), LIMITATIONS ("Only descendants" now points to adopt-external),
USE-CASES (a walkthrough), ARCHITECTURE, CHANGELOG, `.planning` (CRIU-01; Phase 12 notes that `fg` exists).

## My calls (not discussed; change any of them)

- Command names `adopt-external` (roadmap placeholder) and `fg`.
- Joining an existing family by NAME, or creating a lingering one.
- Detach key Ctrl-P Ctrl-Q; `fg` exits with the job's exit code.
- Images deleted on success, kept on failure.
- Trial dump on by default (no flag to skip it).
- Established TCP refused outright rather than attempting with extra privilege.

## Left to the builder

- Session and process-group handling on restore. With `--shell-job`, the restored job must end up as a session
  with the new pty as its controlling terminal. This is the riskiest unknown and is the plan's first task, as a
  throwaway spike against criu 4.2 here.
- How the supervisor runs criu without blocking its poll loop (it can reuse the Phase 7 job pattern).
- Exact wording of messages, JSON keys for the pty in `inspect --json`.

## Precondition for running the tests here

criu 4.2 is installed at `/usr/sbin/criu` without the capability. The user runs the `setcap` line once; focker and
its tests never do.
