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
- **member origins**: `run` (the first command), `adopted` (moved by `cutover`), `orphan` (the kernel reparented it to focker without a cutover, e.g. its parent simply exited), `external` (moved from another terminal by `adopt-external`).
- **family terminal**: a pty whose master the supervisor holds (`run -d -t`, or a terminal job moved by `adopt-external`). Its output goes to `focker logs NAME` until `focker fg NAME` attaches a terminal to it.
- **held fds**: the old parent's ends of a child's pipes and sockets. focker took them over so the child never sees EOF or EPIPE.

---

## focker run

```
focker run [-n NAME] [-d [-t]] [-l] [--] CMD [ARG...]
```

| option | default | meaning |
|---|---|---|
| `-n, --name NAME` | random | family name |
| `-d, --detach` | off | run in the background. Prints NAME and returns once the first command has started. stdin is `/dev/null`; stdout and stderr go to `focker logs NAME` |
| `-t, --tty` | off | with `-d`: run CMD on a family terminal. CMD leads its foreground group, so it sees a terminal (`isatty`, line editing, Ctrl-C); its output goes to `focker logs NAME` until `focker fg NAME`. Without `-d` it is a usage error: a foreground family already has your terminal |
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
- `tcp`: your end of a loopback TCP connection (`127.0.0.0/8` or `::1`) whose other end is the child's CHILDFD. The cutover transcript shows its endpoints, as in `(127.0.0.1:51671 <-> 127.0.0.1:53362)`.

Data focker drained before the handover is in `adopted-CHILD-fdN.log`, so CMD sees only what comes after. See [HOWTO: write a new-parent program](HOWTO.md#write-a-new-parent-program).

## focker fg

```
focker fg NAME
```

Attaches this terminal to the family terminal of NAME. You see the job's output and your keys, Ctrl-C and window size reach it. The terminal goes back to how it was on every way out.

- **Ctrl-P Ctrl-Q** detaches: `fg` exits 0, the job keeps running, and its output goes to `focker logs NAME` again. Ctrl-P followed by any other key passes both keys through.
- **One terminal at a time.** A second `fg` takes the job over; the first prints `taken over by another terminal` and exits 0.
- **When the job ends**, `fg` prints what it printed last, then `NAME: exited N` (or `killed by SIGx`), and exits with the job's status (`128+N` for signal N). `fg` on a family whose terminal job has already ended reports that status the same way.
- `fg` on a family without a terminal exits 1 with `NAME has no terminal`. `fg` needs a terminal of its own: with stdin not a terminal it exits 1.

## focker adopt-external

```
focker adopt-external [-y] [--dry-run] NAME PID
```

Moves the job containing PID, started in another terminal (or by anything else that is not focker), into family NAME. The job is PID's whole process group, so a pipeline moves together, plus every process below it. focker uses criu to checkpoint the job and restore it as the supervisor's child: the same PIDs, the same memory and open files, a new parent. A job that had a terminal gets a family terminal: its output goes to `focker logs NAME` until `focker fg NAME`.

| option | meaning |
|---|---|
| `-y, --yes` | don't ask before the original is stopped and killed |
| `--dry-run` | run every check, including the trial dump, print the plan, and change nothing |

If NAME is running, the job joins it; else a detached family NAME is started for the job, and it ends when the job ends. A terminal job can join only a family that runs detached and has no terminal yet: one terminal job per family.

What happens, in order:
1. **Checks, while the job keeps running.** criu is found (`$FOCKER_CRIU`, else `criu` on `PATH` or in `/usr/sbin`) and has `CAP_CHECKPOINT_RESTORE`. PID is yours, not already in a family, and its job is not the one running focker. No process in the job leads a session (a shell: adopt a job it started instead), and no pipe leads from the job to a process outside it. Then a **trial dump** of each part of the job (`criu dump --leave-running`, images deleted) catches whatever else criu refuses, such as an established TCP connection or a unix socket to a process outside the job. Any failure exits 1 with the reason and `nothing was changed`.
2. **The plan** (`plan: job PGID: roots ...`, `plan: processes ...`, the terminal, the family, the images directory), then `kill the original and restore under NAME? [y/N]`. Without `--yes` stdin must be a terminal.
3. **The move**, in the family's supervisor. Each step prints one `adopt:` line:
   - `adopt: stopped job PGID (N processes)`: every process gets `SIGSTOP`.
   - `adopt: dumped ROOT`, once per root (a process of the job whose parent is outside it; a pipeline has one per stage), into `STATE_DIR/criu/ROOT`. If a dump fails, the job is sent `SIGCONT` and runs on where it was: `adopt: dump failed: REASON; job PGID continues where it was`, exit 1. A job that was in the foreground of its terminal is in the background there now (the stop handed the terminal back to its shell): the line then ends `continues, in the background in its terminal; 'fg' there brings it back`.
   - `adopt: killed the originals (point of no return)`.
   - `adopt: restored ROOT`, once per root, as a child of the supervisor, stopped. Pipes between the roots are recreated with the bytes they held.
   - `adopt: job PGID runs under NAME as process group G`: every process got `SIGCONT`. The images are deleted.

After a successful move the job is in a new process group G. The shell in the old terminal may still list the old group as a job until it next prints a prompt, and when that terminal closes it sends `SIGHUP` to every group it lists. The new group is out of its reach. The job's terminal settings come from the old terminal when the job was in the foreground there (else the defaults), and its window size always does.

If the restore fails after the point of no return, whatever was restored is killed, the images stay in `STATE_DIR/criu/ROOT`, and each root gets `adopt: restore failed; images kept in DIR; retry with: CRIU-ARGV`. The family keeps running (lingering) so the images are not removed with it. Exit 1.

Needs criu with `CAP_CHECKPOINT_RESTORE` (`sudo setcap cap_checkpoint_restore+eip "$(command -v criu)"`, once). focker itself stays unprivileged. Under Yama `ptrace_scope` 1, criu may only attach to its own descendants: focker detects that refusal and prints the `sudo sysctl kernel.yama.ptrace_scope=0` that allows it (and the command to set it back).

## focker ps

```
focker ps [-a] [--json]
```

Lists running families, sorted by name: `NAME PID STATUS CHILDREN COMMAND`. `-a` adds exited ones, with their exit record (`exited 0`, `killed by SIGTERM`, `handed over to ...`).

`--json` prints `{"families": [...]}`, with one object per family in the same order and with the same `-a` filter:
`{"name": "fam", "pid": 4200, "running": true, "status": "running", "children": 2, "cmd": "./runner"}`. For a
family that isn't running, `running` is false, `status` is its exit record (or `gone (no exit record)`),
`children` is null, and `pid` is the last supervisor PID or null.

## focker tree / inspect

```
focker tree NAME
focker inspect [--json] NAME
```

`tree` prints the live process tree under the supervisor, with `(adopted from PID)`, `(orphan, adopted)` or `(moved from another terminal, was under PID)` on direct children. `inspect` lists every member focker has seen (pid, origin, old parent, running or exit status, command) and every held fd (kind, child, child fd, old parent, log path).

`inspect --json` prints the same thing as one JSON document. Every key is always present, with `null` when it doesn't apply. Strings are always valid UTF-8: a byte that isn't, as can happen in a command line, becomes U+FFFD.

```json
{"name": "fam", "supervisor": 4200, "state_dir": "/run/user/1000/focker/fam", "linger": false,
 "members": [{"pid": 4202, "origin": "adopted", "from": 4201, "alive": false, "status": "exited 3",
              "exit_code": 3, "signal": null, "cmd": "./ticker -i 50"}],
 "held_fds": [{"fd": 7, "kind": "pipe-r", "child": 4202, "child_fd": 1, "from": 4201,
               "log": "/run/user/1000/focker/fam/adopted-4202-fd1.log"}]}
```

| key | type | meaning |
|---|---|---|
| `members[].origin` | `"run"`, `"adopted"`, `"orphan"`, `"external"` | started by `run`, moved by `cutover`, handed over by the kernel, or moved by `adopt-external` |
| `members[].from` | int or null | the old parent, for adopted and external members |
| `members[].status` | string | `running`, or the same text as the report (`exited 3`, `killed by SIGTERM`) |
| `members[].exit_code` / `signal` | int / `"SIGx"`, or null | how a dead member ended |
| `held_fds[].fd` | int | focker's own fd number, as used by `release` and `FOCKER_FDS` |
| `held_fds[].kind` | `"pipe-r"`, `"pipe-w"`, `"unix"`, `"tcp"`, `"pty"` | `pty`: the family terminal's master; `child` is its job, `from` the supervisor |
| `held_fds[].log` | string or null | the drain log, or null for an fd that is only held (a `pty` while `fg` is attached) |

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

## focker release

```
focker release NAME FD...
```

Closes fds that focker holds for adopted children. FD is focker's own fd number: the `FD` column of `inspect`, `held_fds[].fd` in `inspect --json`, or the first field of a `FOCKER_FDS` entry. Each one prints `release: closed KIND fd N (child PID fd M)`.
- Releasing a `pipe-w` is how you send a child EOF on the stdin the old parent used to write to.
- Releasing a drained `pipe-r`, `unix` or `tcp` fd closes its log as well. If nobody else holds that end, the child gets `EPIPE`/`SIGPIPE` on its next write.
- An FD focker doesn't hold prints `release: focker holds no fd N`. The other FDs are still released, and the command exits 1.
- A missing or non-numeric FD is a usage error (exit 2).

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
| `adopt-external`: processes in a job | 64 | refused, nothing changed |
| `adopt-external`: roots, and pipes between them | 16 each | refused, nothing changed |
| `adopt-external`: each criu run | 60 s | criu is killed; before the point of no return the job runs on, after it the images are kept |

These are compile-time constants in `src/focker.h` (`MAX_PIDS`, `MAX_STOLEN`, `MAX_MEMBERS`, `MAX_TIDS`, `NAME_MAX_LEN`, `MAX_ROOTS`, `CRIU_TIMEOUT_MS`).

## Environment

| variable | effect |
|---|---|
| `FOCKER_RUNTIME_DIR` | where families live. Default `$XDG_RUNTIME_DIR/focker`, falling back to `/tmp/focker-$UID`. It must be a real directory (not a symlink) owned by you |
| `FOCKER_FREEZE` | default `--freeze` method for `cutover` |
| `FOCKER_CRIU` | the criu binary `adopt-external` uses. Default: `criu` on `PATH`, then `/usr/sbin`, `/usr/local/sbin`, `/sbin` |

Precedence is flag > environment > default.

## Files

```
$FOCKER_RUNTIME_DIR/NAME/
├── control.sock            control socket (mode 0600; requests from other uids are rejected via SO_PEERCRED)
├── pid                     supervisor pid
├── cmd                     first command line
├── focker.log              detached mode only: supervisor log + first command's stdout/stderr (+ the family terminal's output)
├── adopted-PID-fdN.log     output drained from child PID's fd N after a cutover
├── criu/ROOT/              adopt-external images, while a move runs, and kept after a failed restore
└── exit                    written on exit: "exited N", "killed by SIGx" or "handed over to CMD"
```

## Control protocol

The control socket is a unix `SOCK_STREAM` socket. A request is a sequence of NUL-terminated tokens followed by `shutdown(SHUT_WR)`. The reply is `ok\n` or `err\n`, then free text, then close. `handover` replies `exec\n` and its transcript, then keeps the connection open across `execve`: the socket is close-on-exec, so EOF means the exec happened, and `\nerr\n` plus a reason means it failed (`focker handover` then exits 1). Requests are served one at a time, except that a cutover waiting for its old parents (up to `--timeout`) runs in the background: meanwhile `info`, `members`, `inspect` and `attach` are answered and held fds keep draining, while `cutover` (including `--dry-run`), `release`, `stop`, `handover` and `adopt` get `err` and `busy (a cutover is in progress)`. An `adopt` runs criu in the request itself, so other requests wait for it (each criu run is capped at 60 s). The cutover's own reply is sent when it ends.

| request | tokens |
|---|---|
| `info` | prints `name=`, `pid=`, `children=`, `cmd=` lines |
| `members` | one `PID ORIGIN OLDPPID ALIVE` line per member (ORIGIN is `r`, `a`, `o` or `x`) |
| `inspect` | human-readable report |
| `cutover` | `freeze=ptrace\|signal\|none signal=N timeout=MS from=PID steal=0\|1 drain=0\|1 suppress=0\|1 dry_run=0\|1 pid=N...` |
| `stop` | `signal=N` |
| `handover` | the command's argv |
| `attach` | no tokens. Replies `ok\n` with the pty master attached (`SCM_RIGHTS`; none if the job has let go of it) and keeps the connection: later it sends `taken-over\n` when another `attach` takes the terminal, or `exit CODE HOW\n` when the job has ended. Closing the connection detaches |
| `adopt` | `criu=PATH pgid=P root=R... [new=1]`. The supervisor works the job out again from `/proc` and refuses `the job changed since it was checked` if its group or roots differ. `new=1`: the client started this family for the job, so it ends with the job |

The CLI is the supported interface. The protocol is documented for debugging and may change.
