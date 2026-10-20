# focker

**Think docker, but it's meet the Fockers.**

`focker` moves running Linux processes from one parent to another. It freezes the
child, takes over every pipe and socket the old parent shares with it, makes the
old parent exit, checks that the child landed on the new parent, and thaws it.
It can then `execve()` a completely different parent program in its own place,
and that program inherits the children as real children: `getppid()` returns its
PID and its `waitpid()` reaps them.

```
before                         cutover                        handover
focker (subreaper)             focker (subreaper)             newparent   (same PID as focker)
└─ oldparent                   └─ ticker   ← adopted          └─ ticker   ← real child, waitpid() works
   └─ ticker  ─pipe→ oldparent      └─pipe→ focker (drained)       └─pipe→ newparent
```

## Why this needs a tool

Linux has no "change parent" syscall. The kernel rewrites a process's parent at
exactly one moment, when the parent dies. It then hands the orphans to the
nearest ancestor that set `PR_SET_CHILD_SUBREAPER`, or to PID 1. focker is that
ancestor. The cutover is the work of making the old parent die without the
child noticing:

| without focker | with `focker cutover` |
|---|---|
| child's stdout pipe loses its reader → `SIGPIPE` | focker took over the read end and keeps draining it |
| `PR_SET_PDEATHSIG` fires on reparent → child killed | ptrace freeze catches the signal and swallows it |
| old parent's shutdown handler kills its kids | same: the signal is swallowed |
| child lands on PID 1, and nothing you control can `waitpid()` it | child lands on focker, then on your program after `handover` |

## Install

Needs Linux 5.6+ (`pidfd_getfd`), a C11 compiler and glibc 2.32+. There are no other dependencies.

```sh
make            # builds build/focker and the example programs
make test       # 35 unit + 117 integration tests (real processes, no root needed)
make unit       # just the /proc parser and sock_diag unit tests
make demo       # narrated run → cutover → handover walkthrough
sudo make install   # optional: /usr/local/bin/focker (PREFIX=... to change)
```

## Quick start

```sh
# 1. start a family; focker becomes the subreaper and runs the "old parent"
$ focker run -d -n demo -- ./build/oldparent 2
demo
$ focker tree demo
4200 focker[demo]
└─ 4201 ./build/oldparent 2
   ├─ 4202 ./build/ticker
   └─ 4203 ./build/ticker

# 2. look before you leap
$ focker cutover --dry-run demo 4202
plan: adopt 4202 (./build/ticker) from 4201 (./build/oldparent 2)
plan: take over pipe-r fd 3 of 4201 (child 4202 fd 1), draining its output to a log
plan: 4203 (./build/ticker) is also a child of the old parent and will be adopted unfrozen
plan: send SIGTERM to old parent 4201
plan: freeze=ptrace, timeout=10.0s

# 3. cut over
$ focker cutover demo 4202
cutover: froze 4202 (./build/ticker, 1 thread, ptrace)
cutover: took over pipe-r fd 3 of 4201 (child 4202 fd 1), draining its output to a log
cutover: sent SIGTERM to old parent 4201
cutover: adopted 4202 (ppid 4201 -> 4200) in 1 ms
cutover: thawed 4202

# 4. hand the adopted children to a new parent program (exec in place, same PID)
$ focker handover demo -- ./build/newparent
handover: 4200 is now ./build/newparent; it inherits 1 child and 1 fd
```

## Commands

| command | what it does |
|---|---|
| `focker run [-n NAME] [-d] [-l] -- CMD...` | start a family under a subreaper supervisor |
| `focker cutover [opts] NAME PID...` | adopt PIDs away from their parent (alias: `adopt`) |
| `focker handover NAME -- CMD...` | exec CMD in focker's place; it inherits children and fds (alias: `meet-the-parents`) |
| `focker ps [-a]` | list families |
| `focker tree NAME` | process tree, with adopted children marked |
| `focker inspect NAME` | members, origins, exit statuses and held fds |
| `focker logs [-f] [-p PID] NAME` | supervisor log, or a child's drained output |
| `focker freeze / thaw NAME PID...` | SIGSTOP / SIGCONT a family member |
| `focker stop [-s SIG] NAME` | signal the family's direct children |
| `focker rm NAME` | delete an exited family's state |

Every command takes `--help`. Exit codes are `0` for success, `1` for a runtime failure and `2` for a usage error.

## Documentation

- [docs/USAGE.md](docs/USAGE.md): full command and option reference, env vars, files, and the handover contract
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the kernel mechanics work and how focker is built
- [docs/HOWTO.md](docs/HOWTO.md): recipes, such as live-upgrading a parent, writing a new-parent program, and handling stubborn parents
- [docs/LIMITATIONS.md](docs/LIMITATIONS.md): what focker cannot do, and why

## The one rule

**focker can only adopt its own descendants.** That's a kernel rule, not a focker
choice: orphans go to an *ancestor* subreaper. So start the old parent with
`focker run` before you need to move anything. To move a process that was never
started under focker, see the CRIU recipe in [HOWTO](docs/HOWTO.md#processes-not-started-under-focker).
