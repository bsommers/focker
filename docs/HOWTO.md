# focker how-to

- [Live-upgrade a parent without restarting its children](#live-upgrade-a-parent-without-restarting-its-children)
- [Rescue children from a misbehaving parent](#rescue-children-from-a-misbehaving-parent)
- [Write a new-parent program](#write-a-new-parent-program)
- [Parents that won't die, or die loudly](#parents-that-wont-die-or-die-loudly)
- [Children with PR_SET_PDEATHSIG](#children-with-pr_set_pdeathsig)
- [Look before you cut](#look-before-you-cut)
- [Use focker in scripts](#use-focker-in-scripts)
- [Processes not started under focker](#processes-not-started-under-focker)
- [Troubleshooting](#troubleshooting)

---

## Live-upgrade a parent without restarting its children

The situation: a supervisor-style program (a job runner, a REPL host, a build orchestrator) owns long-running children. You want to replace it with a new version, or a different program entirely, without killing the children.

```sh
# from day one, start the parent under focker
focker run -d -n jobs -- ./runner-v1

# upgrade time: move every child of runner-v1 onto focker, then retire v1
OLD=$(focker tree jobs | awk 'NR==2 {print $2}')   # the first command's pid
focker cutover --from "$OLD" jobs
# hand the children to v2; it gets them via FOCKER_CHILDREN / FOCKER_FDS
focker handover jobs -- ./runner-v2 --adopt
```

If v2 can't adopt (it only knows how to spawn), stop after `cutover`. focker keeps supervising the children, draining their output and recording their exit statuses, and `focker inspect jobs` shows them.

## Rescue children from a misbehaving parent

A parent is leaking memory or wedged, but its children are doing useful work.

```sh
focker cutover --dry-run fam 4242        # check what will be taken over
focker cutover -s KILL fam 4242          # a wedged parent may not handle TERM
focker logs -p 4242 fam                  # the child's output, now captured by focker
```

## Write a new-parent program

After `focker handover NAME -- CMD`, CMD is the process focker was. Its job is to reap the children it inherited and to use the fds it was handed.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    /* FOCKER_CHILDREN=4202,4310 : already our children; getppid() in them == getpid() here */
    printf("adopted: %s\n", getenv("FOCKER_CHILDREN") ? getenv("FOCKER_CHILDREN") : "");

    /* FOCKER_FDS=9:4202:pipe-r:1 : fd 9 here reads what child 4202 writes to its fd 1 */
    char *fds = strdup(getenv("FOCKER_FDS") ? getenv("FOCKER_FDS") : "");
    for (char *t = strtok(fds, ","); t; t = strtok(NULL, ",")) {
        int fd, child, childfd; char kind[16];
        if (sscanf(t, "%d:%d:%15[^:]:%d", &fd, &child, kind, &childfd) == 4)
            printf("fd %d is %s for child %d fd %d\n", fd, kind, child, childfd);
    }

    /* reap like any parent; future orphans below us arrive here too
       (PR_SET_CHILD_SUBREAPER survives execve) */
    int st; pid_t pid;
    while ((pid = wait(&st)) > 0)
        printf("child %d done\n", pid);
    return 0;
}
```

What to know:
- **Close what you don't need.** A held `pipe-w` keeps the child's stdin open forever. Close it to send the child EOF. Without a handover, `focker release NAME FD` does the same from the outside.
- **Drain what you keep.** A `pipe-r` you never read fills up (64 KiB by default), and then the child blocks on write.
- **No focker after this.** The control socket is gone, so `focker` commands report the family as not running. `ps -a` shows `handed over to CMD`.
- `examples/newparent.c` is a complete version that relays output with `poll()`.

## Parents that won't die, or die loudly

| parent behavior | what to use |
|---|---|
| exits cleanly on TERM | the default |
| ignores TERM | `-s KILL` |
| needs a graceful shutdown through its own API | ask it to exit, then `focker cutover --wait -t 60 ...` |
| kills its children on shutdown (`kill(0)`, `trap 'kill $!'`) | the default ptrace freeze swallows it. Check the transcript for `swallowed SIGTERM` |
| kills its children with SIGKILL on shutdown | nothing can intercept SIGKILL. Use `-s KILL` so the parent never runs its shutdown path |
| never exits and you can't kill it | not possible: the kernel only reparents on parent death. `--timeout` rolls back cleanly |

## Children with PR_SET_PDEATHSIG

`PR_SET_PDEATHSIG` fires when the child's parent *thread* exits, which is exactly what a cutover does. With the default `--freeze ptrace`, the signal arrives while the child is frozen and is swallowed (`cutover: swallowed SIGHUP ...`). With `--freeze signal`, it is delivered on thaw, and focker warns about it. If the death signal is `SIGKILL`, the child dies whatever you do, and focker reports `died during the cutover`.

After the cutover, the death signal stays armed. If focker (or the new parent after a handover) exits, the child gets it again.

## Look before you cut

```sh
focker tree fam                      # who's where
focker freeze fam 4242               # optional: pause it while you look around
ls -l /proc/4242/fd                  # what it's connected to
focker cutover --dry-run fam 4242    # targets, fds to take, bystanders, signal
focker cutover fam 4242              # ptrace freeze works on a SIGSTOP'd process too
focker thaw fam 4242                 # undo your manual freeze
```

## Use focker in scripts

- `focker run -d` prints only the family name on stdout. Capture it with `NAME=$(focker run -d -- cmd)`.
- Exit codes are `0` for success, `1` for a runtime failure and `2` for a usage error. A failed cutover rolls back before it exits 1.
- `focker cutover` transcript lines start with `cutover: ` and have stable shapes, so grep for `^cutover: adopted`.
- Point `FOCKER_RUNTIME_DIR` at a private directory to isolate test runs. `tests/run.sh` does this.

## Processes not started under focker

`focker cutover` can only adopt descendants. For a job that already runs elsewhere, say in another terminal, use `focker adopt-external`. It checkpoints the job with criu and restores it as a child of a family's supervisor:

```sh
sudo setcap cap_checkpoint_restore+eip "$(command -v criu)"   # once
focker adopt-external --dry-run work 4242   # checks, trial dump, plan; changes nothing
focker adopt-external work 4242             # asks, then moves PID 4242's whole job
focker logs -f work                         # its output, until you
focker fg work                              # attach this terminal (Ctrl-P Ctrl-Q detaches)
```

PID can be any process of the job; its process group moves, so a pipeline moves together. If the trial dump fails, the reason is criu's (for example `Connected TCP socket`) and nothing was changed. Under Yama `ptrace_scope` 1, focker prints the `sysctl` that lets criu attach. See [USAGE](USAGE.md#focker-adopt-external) for each step and [LIMITATIONS](LIMITATIONS.md) for what cannot move.

What `adopt-external` does is the classic criu recipe, with the details done for you. By hand it looks like this; criu recreates the process tree under whoever runs the restore:

```sh
sudo criu dump    -t "$PID" -D img --shell-job          # freezes, writes images, kills the original
sudo criu restore -D img --shell-job --restore-sibling  # restored tree becomes a child of criu's parent
```

Run the restore from (or as) the new parent: with `--restore-sibling`, the restored process becomes a sibling of `criu`, which makes it a child of whoever ran `criu`. Caveats:
- the original PIDs must be free (or restore into a new PID namespace)
- pipes and sockets to the old parent need `--inherit-fd` or `--ext-unix-sk`
- TCP needs `--tcp-established`
- it requires `CAP_CHECKPOINT_RESTORE` or root

## Troubleshooting

| symptom | cause and fix |
|---|---|
| `X is not in family NAME` | X wasn't started below `focker run`. See the previous section |
| `cannot ptrace X: Operation not permitted` | another tracer (gdb, strace) is attached, or `kernel.yama.ptrace_scope` is 2 or 3. Detach it, or use `-f signal` and accept its caveats |
| `timed out ... the old parent is still alive` | the parent ignores the signal. Use `-s KILL`, or `--wait` with a longer `-t` |
| `X went to Y, not focker` | a subreaper sits between them (systemd --user, tini, another focker). Run focker below it |
| `X died during the cutover` | almost always `PR_SET_PDEATHSIG=SIGKILL`. Look for `prctl(PR_SET_PDEATHSIG, SIGKILL)` in the program's source, since there's no /proc field for it |
| a bystander died of SIGPIPE | it wasn't a target, so its pipe wasn't taken over. Use `--from PPID` |
| child blocks after cutover | `--no-drain` was used, or the child reads from a held `unix` peer that will never send anything |
| `no running family` right after `run -d` | check `focker logs NAME`, and `ps -a` for the exit record |
