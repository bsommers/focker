#!/usr/bin/env python3
"""Start a shell-style job for the adopt-external tests: the stages of a pipeline
in one new process group, this launcher staying their parent like a shell.

usage: mkjob.py [--no-ptracer] [--leak] PIDFILE CMD [ARG...] ['|' CMD [ARG...]]...

Each stage allows any process to ptrace it (PR_SET_PTRACER_ANY), as criu needs
under Yama ptrace_scope 1, unless --no-ptracer. The last stage's output goes to
/dev/null, or with --leak to a pipe this launcher reads (a pipe leading out of
the job). Writes the stage pids, space-separated, to PIDFILE, then waits.
"""
import ctypes
import os
import sys

PR_SET_PTRACER = 0x59616D61
PR_SET_PTRACER_ANY = ctypes.c_ulong(-1)


def main():
    args = sys.argv[1:]
    ptracer, leak = True, False
    while args and args[0].startswith("--"):
        if args[0] == "--no-ptracer":
            ptracer = False
        elif args[0] == "--leak":
            leak = True
        args = args[1:]
    pidfile, args = args[0], args[1:]
    stages, cur = [], []
    for a in args:
        if a == "|":
            stages.append(cur)
            cur = []
        else:
            cur.append(a)
    stages.append(cur)

    libc = ctypes.CDLL(None, use_errno=True)
    pids, pgid, stdin = [], 0, os.open(os.devnull, os.O_RDONLY)
    out_r = None
    for i, argv in enumerate(stages):
        last = i == len(stages) - 1
        if not last:
            r, w = os.pipe()
        elif leak:
            out_r, w = os.pipe()
        else:
            r, w = None, os.open(os.devnull, os.O_WRONLY)
        pid = os.fork()
        if pid == 0:
            os.setpgid(0, pgid)
            if ptracer:
                libc.prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0)
            os.dup2(stdin, 0)
            os.dup2(w, 1)
            os.closerange(3, 256)
            os.execvp(argv[0], argv)
        os.setpgid(pid, pgid or pid)  # as a shell does, against the race with the child
        pgid = pgid or pid
        pids.append(pid)
        os.close(stdin)
        os.close(w)
        if not last:
            stdin = r
    with open(pidfile + ".tmp", "w") as f:
        f.write(" ".join(map(str, pids)) + "\n")
    os.rename(pidfile + ".tmp", pidfile)
    if out_r is not None:  # keep the pipe open and drained, from outside the job
        while os.read(out_r, 65536):
            pass
    for _ in pids:
        try:
            os.wait()
        except ChildProcessError:
            break


if __name__ == "__main__":
    main()
