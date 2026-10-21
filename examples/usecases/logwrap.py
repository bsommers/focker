#!/usr/bin/env python3
"""logwrap: a stand-in for the wrapper scripts that sit in front of real jobs
(log shippers, CI job runners, batch drivers). It runs one job and writes the
job's stdout to LOG, each line tagged with the wrapper's version.

  logwrap.py VERSION LOG -- CMD...   v1: start CMD and wrap it
  logwrap.py VERSION LOG             v2: started by 'focker handover'; take over
                                     the job from FOCKER_CHILDREN / FOCKER_FDS

In v2 mode it first copies what focker drained between the cutover and the
handover (adopted-PID-fdN.log in FOCKER_STATE_DIR), so no line is lost, then
keeps reading the inherited pipe. Like any parent, it reaps its children.
"""
import os
import subprocess
import sys


def pump(version, src, log):
    with open(log, "a", buffering=1) as out:
        for line in src:
            out.write(f"[{version}] {line.decode(errors='replace')}")


def reap_all():
    while True:
        try:
            pid, status = os.waitpid(-1, 0)
        except ChildProcessError:
            return
        print(f"logwrap: reaped {pid}: exit {os.waitstatus_to_exitcode(status)}", flush=True)


def main():
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    version, log = sys.argv[1], sys.argv[2]
    if len(sys.argv) > 4 and sys.argv[3] == "--":
        job = subprocess.Popen(sys.argv[4:], stdout=subprocess.PIPE, bufsize=0)
        pump(version, job.stdout, log)
        return job.wait()

    # v2: adopt what focker hands us. FD:CHILD:KIND:CHILDFD, comma-separated.
    state = os.environ["FOCKER_STATE_DIR"]
    for entry in filter(None, os.environ.get("FOCKER_FDS", "").split(",")):
        fd, child, kind, child_fd = entry.split(":")
        if kind not in ("pipe-r", "unix", "tcp"):
            continue
        drained = os.path.join(state, f"adopted-{child}-fd{child_fd}.log")
        if os.path.exists(drained):
            with open(drained, "rb") as gap:
                pump(f"{version}, drained by focker", gap, log)
        pump(version, os.fdopen(int(fd), "rb", buffering=0), log)
    reap_all()
    return 0


if __name__ == "__main__":
    sys.exit(main())
