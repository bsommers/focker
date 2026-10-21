# focker usage reference

```
focker <command> [options] [args]
```

Exit codes for every command:

| code | meaning |
|---|---|
| 0 | success |
| 1 | runtime failure: no such family, cutover failed or rolled back, process not in the family, ... (one-line diagnostic on stderr) |
| 2 | usage error: unknown command, bad option or value, invalid family name |

`focker run` in the foreground is the exception: it exits with the first command's status (or `128+N` if signal N killed it, and `127` if the command could not be started).

Output goes to stdout and diagnostics to stderr. When a cutover fails, its whole transcript goes to stderr.

---

## Concepts

- **family**: one supervisor process plus everything below it, identified by a NAME (`[A-Za-z0-9_.-]`, at most 48 characters, not starting with `.` or `-`). If you don't give a name, `run` picks one like `suspicious_jack`.
- **supervisor**: the `focker run` process. It sets `PR_SET_CHILD_SUBREAPER`, so every orphan in its subtree is reparented to it, and it serves a control socket.
- **first command**: the CMD given to `run`. It is usually the "old parent" you plan to replace.
- **member origins**: `run` (the first command), `adopted` (moved by `cutover`), `orphan` (the kernel reparented it to focker without a cutover, e.g. its parent simply exited).
- **held fds**: the old parent's ends of a child's pipes and sockets. focker took them over so the child never sees EOF or EPIPE.

---

## focker run

```
focker run [-n NAME] [-d] [-l] [--] CMD [ARG...]
```

| option | default | meaning |
|---|---|---|
| `-n, --name NAME` | random | family name |
| `-d, --detach` | off | run in the background. Prints NAME and returns once the first command has started. stdin is `/dev/null`; stdout and stderr go to `focker logs NAME` |
| `-l, --linger` | off | keep the supervisor alive when the family has no processes left (end it with `focker stop`) |

Option parsing stops at the first non-option, so `focker run -n x ls -la` works without `--`.

Foreground mode keeps the terminal. Ctrl-C reaches the whole foreground process group as usual, and focker does not re-send it. Signals sent to the supervisor with `kill(1)` (TERM, INT, HUP, QUIT) *are* forwarded to its direct children.

Without `--linger`, the supervisor exits once it has no children. That includes adopted children and orphans the kernel gave it, so the family outlives the first command if anything is still running.

Fails with exit 1 if the name is taken. If an exited family holds the name, `focker rm NAME` it first.

## focker cutover

```
focker cutover [options] NAME PID...
focker cutover [options] --from PPID NAME [PID...]
```

Adopts each PID from its current parent onto the supervisor.

| option | default | meaning |
|---|---|---|
| `-f, --freeze METHOD` | `ptrace` (or `$FOCKER_FREEZE`) | `ptrace`: `PTRACE_SEIZE` + `PTRACE_INTERRUPT` on every thread. The child can't see it, and it can swallow signals. `signal`: `SIGSTOP`/`SIGCONT`, which the child and its parent can observe and which cannot swallow anything. `none`: don't freeze |
| `-s, --signal SIG` | `TERM` | signal for the old parent. Takes a name (`KILL`, `SIGKILL`) or a number |
| `-w, --wait` | off | send nothing; wait for the old parent to exit on its own (e.g. after you tell it to via its own API) |
| `-t, --timeout SEC` | `10` | 0.1 to 3600. If the old parent is still alive after SEC, everything is rolled back |
| `--from PPID` | none | add every current child of PPID to the targets |
| `--no-steal` | off | don't take over the old parent's pipes and sockets |
| `--no-drain` | off | take them over but don't read from them (the child blocks once the pipe buffer fills) |
| `--no-suppress` | off | with `ptrace`, deliver signals that arrived during the cutover instead of swallowing them |
| `-n, --dry-run` | off | print the plan (targets, fds, bystanders, signal) and change nothing |

Rules and behavior:

- Each PID must be a descendant of the supervisor (exit 1 otherwise). If a PID is already a direct child, it is skipped.
- A PID can't be both a target and the parent of another target. Cut over one generation at a time.
- Grandchildren work. Cutting over a grandchild kills *its* parent, and the kernel sends the grandchild straight to focker, skipping the generations in between (unless one of them is itself a subreaper, which is reported).
- **Bystanders:** when the old parent dies, *all* of its children are orphaned. Children you didn't name are adopted too and reported as `also adopted`, but they are not frozen and their fds are not taken over. Use `--from PPID` to include them.
- Swallowed signals: with `--freeze ptrace`, any signal that becomes pending for a target between freeze and thaw is dropped, except `SIGCHLD`, `SIGCONT`, `SIGWINCH` and `SIGURG`. Each one is reported as `swallowed SIGx`. `SIGKILL` cannot be intercepted. Job-control stops (`SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`) are kept, so a target stopped during the cutover is still stopped afterwards. A signal that was already pending before the freeze is reported but not swallowed: the kernel merges a second copy into it, so a re-send can't be detected.
- On timeout or freeze failure, the cutover rolls back: fds it took are closed, the targets are thawed, and they stay where they were. Exit 1.

The transcript lines are stable and greppable:

```
cutover: froze PID (CMD, N threads, ptrace)
cutover: took over KIND fd N of OLDPID (child PID fd N), draining its output to a log | held open
cutover: sent SIG to old parent OLDPID
cutover: adopted PID (ppid OLD -> NEW) in N ms
cutover: swallowed SIGx sent to PID during the cutover
cutover: SIGx reached PID during the cutover; it was kept, so PID will stop
cutover: SIGx was already pending for PID before the freeze; a copy sent during the cutover can't be told apart from it and was not swallowed
cutover: PID exited before it could be frozen
cutover: PID has more than 64 children; name at most 64 of them
cutover: taking N more fds would hold more than 128 (holding M); nothing was changed
cutover: thawed PID
cutover: also adopted PID (CMD), orphaned by the same exit
```

## focker handover

```
focker handover NAME [--] CMD [ARG...]
```

Replaces the supervisor with CMD via `execve()`. The PID stays the same, so:

- every current child of the supervisor becomes a real child of CMD (`getppid()` in the child returns CMD's PID, and CMD's `waitpid()` reaps them)
- `PR_SET_CHILD_SUBREAPER` stays set (it survives `execve`), so future orphans keep landing on CMD
- held fds stay open in CMD, at the fd numbers listed in `FOCKER_FDS`
- CMD inherits the supervisor's environment, cwd and stdio. In detached mode its stdout goes to `focker logs NAME`

CMD is resolved against `PATH` before anything is torn down. If it can't be found, the handover is refused (exit 1) and nothing changes. If `execve` itself fails, focker logs the error and rebuilds its control socket. The family keeps running under focker.

After a successful handover, the family no longer answers focker commands. `focker ps -a` shows it as `handed over to CMD`.

### Environment passed to CMD

| variable | format | example |
|---|---|---|
| `FOCKER_CHILDREN` | comma-separated pids of every direct child | `4202,4310` |
| `FOCKER_FDS` | comma-separated `FD:CHILD:KIND:CHILDFD` | `9:4202:pipe-r:1,10:4202:pipe-w:0` |
| `FOCKER_NAME` | family name | `demo` |
| `FOCKER_STATE_DIR` | family state directory | `/run/user/1000/focker/demo` |

`KIND` is one of:

- `pipe-r`: a read end. The child writes to the pipe at its fd CHILDFD, and you read it here.
- `pipe-w`: a write end. You write here, and the child reads at CHILDFD.
- `unix`: your end of a unix socket connected to the child's CHILDFD.

Data focker drained before the handover is in `adopted-CHILD-fdN.log`, so CMD sees only what comes after. See [HOWTO: write a new-parent program](HOWTO.md#write-a-new-parent-program).

## focker ps

```
focker ps [-a]
```

Lists running families, sorted by name: `NAME PID STATUS CHILDREN COMMAND`. `-a` adds exited ones, with their exit record (`exited 0`, `killed by SIGTERM`, `handed over to ...`).

## focker tree / inspect

```
focker tree NAME
focker inspect NAME
```

`tree` prints the live process tree under the supervisor, with `(adopted from PID)` or `(orphan, adopted)` on direct children. `inspect` lists every member focker has seen (pid, origin, old parent, running or exit status, command) and every held fd (kind, child, child fd, old parent, log path).

## focker logs

```
focker logs [-f] [-p PID] NAME
```

Without `-p`, prints `focker.log`. A detached family's supervisor messages and the first command's output go there, and so does whatever the new parent writes after a handover. With `-p PID`, prints the output focker drained from PID's taken-over pipes and sockets. `-f` follows until the family exits.

## focker freeze / thaw

```
focker freeze NAME PID...
focker thaw NAME PID...
```

`SIGSTOP` / `SIGCONT` on family members. `freeze` waits up to 2s for the process to show state `T`. This is handy for pausing a child while you look around before a cutover. A cutover with `--freeze ptrace` works on an already-stopped process; thaw it afterwards.

## focker stop

```
focker stop [-s SIG] NAME
```

Sends SIG (default `TERM`) to the supervisor's direct children and clears `--linger`. The supervisor exits once its last child is gone.

## focker rm

```
focker rm NAME
```

Deletes an exited family's state directory. It refuses (exit 1) while the family is running.

---

## Limits

| limit | value | what happens at the limit |
|---|---|---|
| targets per cutover | 64 | more named PIDs is a usage error (exit 2). `--from` a parent with more than 64 children is refused (exit 1), so name up to 64 of them |
| held fds per family | 128 | a cutover that would go past it is refused before anything is frozen. Slots of closed fds are reused |
| remembered members per family | 256 | the oldest *dead* member is forgotten. Running members are never evicted |
| threads per frozen process | 256 | the cutover fails and rolls back |
| family name | 48 characters, `[A-Za-z0-9_.-]`, no leading `.` or `-` | usage error |

These are compile-time constants in `src/focker.h` (`MAX_PIDS`, `MAX_STOLEN`, `MAX_MEMBERS`, `MAX_TIDS`, `NAME_MAX_LEN`).

## Environment

| variable | effect |
|---|---|
| `FOCKER_RUNTIME_DIR` | where families live. Default `$XDG_RUNTIME_DIR/focker`, falling back to `/tmp/focker-$UID`. It must be a real directory (not a symlink) owned by you |
| `FOCKER_FREEZE` | default `--freeze` method for `cutover` |

Precedence is flag > environment > default.

## Files

```
$FOCKER_RUNTIME_DIR/NAME/
├── control.sock            control socket (mode 0600; requests from other uids are rejected via SO_PEERCRED)
├── pid                     supervisor pid
├── cmd                     first command line
├── focker.log              detached mode only: supervisor log + first command's stdout/stderr
├── adopted-PID-fdN.log     output drained from child PID's fd N after a cutover
└── exit                    written on exit: "exited N", "killed by SIGx" or "handed over to CMD"
```

## Control protocol

The control socket is a unix `SOCK_STREAM` socket. A request is a sequence of NUL-terminated tokens followed by `shutdown(SHUT_WR)`. The reply is `ok\n` or `err\n`, then free text, then close. Requests are served one at a time.

| request | tokens |
|---|---|
| `info` | prints `name=`, `pid=`, `children=`, `cmd=` lines |
| `members` | one `PID ORIGIN OLDPPID ALIVE` line per member (ORIGIN is `r`, `a` or `o`) |
| `inspect` | human-readable report |
| `cutover` | `freeze=ptrace\|signal\|none signal=N timeout=MS from=PID steal=0\|1 drain=0\|1 suppress=0\|1 dry_run=0\|1 pid=N...` |
| `stop` | `signal=N` |
| `handover` | the command's argv |

The CLI is the supported interface. The protocol is documented for debugging and may change.
