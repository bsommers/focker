# focker use cases

focker started as a crazy idea: give a running process a new parent. In day-to-day operations it boils down
to one ability. **You can change your mind about a job's parent after the job has started.** That parent
might be a wrapper, a deadline, an orchestrator or a runner, and you can retire, replace or kill it without
restarting the job, and without losing its output or its exit status.

The cost is one habit: start long or precious work under focker.

```sh
focker run -d -n nightly-import -- ./import.sh      # instead of: ./import.sh &
```

An idle supervisor uses about 50 µs of CPU per second ([ARCHITECTURE §5.6](ARCHITECTURE.md)). It also does useful
work you get for nothing: every orphan in the tree is reaped and its exit status recorded (`focker inspect`,
`inspect --json`), and output from fds it takes over is logged.

Each use case below marked **walkthrough** has a script in [`examples/usecases/`](../examples/usecases/). Each
script is also a check: it exits non-zero if the claim didn't hold. Run them all with `make usecases`.

| # | situation | what focker does | status |
|---|---|---|---|
| 1 | a job will outlive its deadline (`timeout`, cron/CI step limits) | removes the deadline, keeps the job and its exit status | walkthrough |
| 2 | an orchestrator hung, and its worker is blocked on a full pipe | takes over the pipe, kills the orchestrator, drains the worker | walkthrough |
| 3 | a wrapper (log shipper, runner) needs replacing mid-job | v1 retired, v2 adopts the job as a real child, no line lost | walkthrough |
| 4 | a consistent backup of files a job keeps writing | pause the writer, snapshot, resume | walkthrough |
| 5 | upgrading a home-grown job runner or queue worker without draining | cutover + handover to the new runner | recipe |
| 6 | knowing how a fire-and-forget job ended, after its launcher died | orphans land on focker, not init, and their status is kept | recipe |
| 7 | leaving a shell or SSH session without killing what it started | works for jobs whose output goes to pipes or files, not a tty | partial |

## 1. Outlive a deadline (walkthrough: `rescue-from-timeout.sh`)

A data migration started under `timeout 2h` (or a cron wrapper, or a CI step limit) and it's clearly going to
need 3. Killing the timeout doesn't help. On the machine these walkthroughs were built on, `timeout` (uutils
coreutils 0.10.0) sets the parent-death signal on its child, so `kill -9 <timeout>` kills the job too. It also
traps SIGTERM, forwards it to the job and waits for the job. Check what your `timeout` does before relying on
`kill -9`.

```sh
focker cutover -s KILL --from "$TIMEOUT_PID" migration     # -s KILL: a TERM would be forwarded and waited on
focker inspect migration                                   # later: "adopted ... exited 0"
```

The cutover swallows the parent-death signal (the transcript says `swallowed SIGTERM`), the job is adopted by
the supervisor, and its exit status shows up in `inspect` when it finishes. Under init the status would have
been lost.

## 2. Unwedge a hung orchestrator (walkthrough: `unwedge-hung-parent.sh`)

A Python ETL driver, a build tool or a test harness starts workers with `stdout=PIPE`, and then it hangs. It
ignores SIGTERM and stops reading. Each worker fills its 64 KiB pipe and blocks in `write()`
(`wchan anon_pipe_write`). Killing the driver gives the workers SIGPIPE or EPIPE on their next write.

```sh
focker cutover --dry-run -s KILL --from "$DRIVER" etl     # shows the pipe it will take over
focker cutover -s KILL --from "$DRIVER" etl
focker logs -f -p "$WORKER" etl                           # the worker's output, now drained by focker
```

The worker is unblocked at once and keeps its exact output stream. The 64 KiB that sat in the pipe buffer
is the first thing in the log. Data the driver had already read into its own userspace buffers is gone with
the driver: focker only has what the kernel still holds.

## 3. Replace a wrapper mid-job (walkthrough: `upgrade-log-wrapper.sh`)

Long jobs often sit behind a wrapper: something that ships their logs, reports their progress or enforces
policy. When the wrapper needs a fix or new config, the usual answer is to restart the job. With focker:

```sh
focker cutover --from "$WRAPPER_V1" ship                  # v1 retired; focker drains the job's output meanwhile
focker handover ship -- ./wrapper v2                      # v2 runs in focker's place, with the job as its child
```

v2 receives `FOCKER_CHILDREN` and `FOCKER_FDS` (the read end of the job's stdout) and the state directory
holding what focker drained in between. The walkthrough's [`logwrap.py`](../examples/usecases/logwrap.py) is
a complete new-parent program in about 60 lines of Python. It copies the drained gap, keeps reading the
inherited pipe, and reaps the job with `os.waitpid(-1, 0)`. The tick numbers in the shipped log are continuous
across v1, the gap and v2.

## 4. Pause for a consistent snapshot (walkthrough: `pause-for-snapshot.sh`)

A backup of files a job keeps writing is only consistent if the writer holds still.

```sh
focker freeze db-load "$PID" && lvcreate -s -n snap -L 1G vg/data && focker thaw db-load "$PID"
```

`freeze` is SIGSTOP and waits until the process really is stopped (state `T`). The walkthrough checks that
the file didn't change during the copy and that the writer resumed where it left off. This is the cheapest use
of focker: no cutover at all. A job that must not see a stop (a job-control-aware shell, or a parent that
reacts to SIGCHLD stops) is better paused with a ptrace freeze. That is what a cutover does internally, but it
isn't exposed as a command yet.

## 5. Upgrade a job runner without draining it (recipe)

Runner agents are the classic case. Restarting gitlab-runner kills the jobs it is running, so the documented
practice is a graceful stop: SIGQUIT and a `TimeoutStopSec` of up to two hours, or checking the API for running
jobs before restarting. For a **home-grown** runner or queue worker that you control, focker replaces the
drain:

```sh
focker run -d -n runner -- ./runner-v1
# upgrade time
focker cutover --from "$(pgrep -P "$(cat "$FOCKER_RUNTIME_DIR/runner/pid")")" runner
focker handover runner -- ./runner-v2 --resume-from-env     # v2 reads FOCKER_CHILDREN / FOCKER_FDS
```

The job processes never stop. v2 has to know how to pick up a running job: its PID, its pipes, and whatever
bookkeeping v1 kept. For an off-the-shelf runner like gitlab-runner, the coordinator's view of the job lives in
the old runner process and is lost with it, so focker isn't a drop-in fix there. See
[HOWTO: write a new-parent program](HOWTO.md#write-a-new-parent-program).

## 6. Know how fire-and-forget jobs ended (recipe)

Scripts that start background work and exit (`cmd &`, `nohup`, daemonizing tools) leave the work to init,
which reaps it and discards the status. Under `focker run --linger`, every orphan in the tree lands on the
supervisor instead:

```sh
focker run -d --linger -n batch -- ./kick-off-everything.sh
focker inspect --json batch | jq '.members[] | {pid, cmd, status}'
```

A member's name is what `/proc` showed when it was reaped, so a subshell appears as `[sh]`. This works with no
cutover at all. focker is acting as a subreaper with a memory, much like `tini` or
`dumb-init` in a container, plus a queryable record.

## 7. Leave a session without killing what it started (partial)

`disown` and `nohup` must be decided before you need them, or they leave output going to a dead terminal.
`reptyr` moves a process to a new terminal with ptrace. focker covers the case where the job's stdin and stdout
are pipes or files, for example `./job | tee out.log` started from a shell that runs under
`focker run -n work -- bash`. This recipe isn't covered by a walkthrough yet:

```sh
focker cutover work "$JOB"      # from another terminal, before closing the session
```

A cutover doesn't cover a job writing straight to the terminal: ptys aren't taken over
([LIMITATIONS](LIMITATIONS.md)), and the tty hangup still reaches the job. For that, move the job with
`adopt-external` (next section).

## 8. Pick up a job from another terminal (recipe)

You started something in terminal A without focker, and now you want it somewhere else: terminal A is about
to close, or you are on another machine's ssh session (terminal B). `adopt-external` checkpoints the job with
criu and restores it under a family, on a terminal focker holds. Its output goes to the family log until you
attach.

In terminal A, the job runs in the background (or Ctrl-Z, `bg`):

```sh
$ ./build/ticker -i 1000 &
[1] 4242
```

In terminal B:

```sh
$ focker adopt-external work 4242
plan: job 4242: roots 4242
plan: processes 4242
plan: terminal /dev/pts/3
plan: family work (new)
plan: images in /run/user/1000/focker/work/criu
kill the original and restore under work? [y/N] y
adopt: stopped job 4242 (1 process)
adopt: dumped 4242
adopt: killed the originals (point of no return)
adopt: restored 4242
adopt: job 4242 runs under work as process group 4250 on a new terminal ('focker fg' it)
$ focker logs work        # what it printed since the move
$ focker fg work          # attach; Ctrl-C reaches it, Ctrl-P Ctrl-Q detaches
```

The job keeps its PID and its memory; its parent is now the family's supervisor, and closing terminal A no
longer reaches it. A pipeline moves together: name any of its PIDs. This needs criu with
`CAP_CHECKPOINT_RESTORE`, and under Yama `ptrace_scope` 1 a sysctl that focker names. A job with a TCP
connection open, or a pipe to a process outside it, can't move; `--dry-run` tells you before anything is
touched ([LIMITATIONS](LIMITATIONS.md)).

## Not a fit

- **Processes that criu can't move.** The kernel only reparents to an ancestor, so a job not started under
  focker moves only by checkpoint and restore (section 8). An established TCP connection, or a pipe or socket
  to a process outside the job, rules that out. Use reptyr or tmux there.
- **`systemctl restart` of a unit focker runs inside.** systemd kills the unit's whole cgroup
  (`KillMode=control-group`), adopted children included. Give each family its own unit, or don't restart the
  unit it lives in.
- **Other hosts, other users, `ptrace_scope` 2 or 3.** See [LIMITATIONS](LIMITATIONS.md).

## Lessons from building the walkthroughs

- **A parent that traps TERM and waits for its children** (`timeout`, many wrappers) never exits while the
  child is frozen, so the cutover times out and rolls back. Use `-s KILL`. A dry run doesn't detect this yet.
- **Process-group signals reach unfrozen grandchildren.** `timeout` signals its whole group. The cutover
  freezes the targets, not their children, so a target's own children (here, its `sleep`) can still be hit.
  Adopt the whole subtree, or pick a signal the parent can't forward.
- **Ignored signals are inherited across `exec`.** A worker started by a parent that ignored SIGTERM ignores it
  too, so `focker stop` (TERM by default) may not stop it. Use `focker stop -s KILL`.
- **Only kernel-held data survives a parent.** Whatever the old parent had already read into its own buffers
  goes with it.

Sources: [gitlab-runner service docs (graceful stop)](https://docs.gitlab.com/runner/configuration/init/),
[GitLab forum: restarting a runner safely](https://forum.gitlab.com/t/how-to-restart-gitlab-runner-safely/45128),
[reptyr(1)](https://manpages.debian.org/jessie/reptyr/reptyr.1.en.html).
