#!/usr/bin/env bash
# Use case: an orchestrator (an ETL driver, a Python harness, a build tool)
# has hung. It ignores SIGTERM, and it stopped reading its worker's stdout, so
# the worker is blocked writing to a full pipe. Killing the orchestrator would
# kill the worker's output (SIGPIPE on the next write). focker takes over the
# read end, kills the orchestrator, and drains the worker's output to a log.
set -eu
# shellcheck source=examples/usecases/lib.sh
. "$(dirname "$0")/lib.sh"
need_python

NAME=etl
FAMILIES+=("$NAME")
say "A hung orchestrator: it read 50 rows from its worker, then stopped reading"
# shellcheck disable=SC2016  # the orchestrator's own code
show "$F" run -d -n "$NAME" -- python3 -c '
import signal, subprocess, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
w = subprocess.Popen(["sh", "-c", "i=0; while :; do i=$((i+1)); echo \"row $i\"; done"], stdout=subprocess.PIPE)
for _ in range(50):
    w.stdout.readline()
time.sleep(10**6)'
sleep 1.5
ORCH=$(kids_of "$(sup_of "$NAME")" | head -1)
W=$(kids_of "$ORCH" | head -1)
show ps -o pid,stat,wchan:20,cmd -p "$W"
[ "$(ps -o wchan= -p "$W")" = anon_pipe_write ] || echo "(the worker is not in anon_pipe_write on this kernel; continuing)"

say "Look first, then cut over with SIGKILL (the orchestrator ignores TERM)"
show "$F" cutover --dry-run -s KILL --from "$ORCH" "$NAME"
show "$F" cutover -s KILL --from "$ORCH" "$NAME"

LOG=$FOCKER_RUNTIME_DIR/$NAME/adopted-$W-fd1.log
size() { stat -c %s "$LOG"; }
s1=$(size); sleep 1; s2=$(size)
printf 'drained log: %s bytes, one second later %s bytes\n' "$s1" "$s2"
[ "$s2" -gt "$s1" ] || die "the worker is still blocked"
ok "the worker is unblocked and its output is being drained"
show "$F" logs -p "$W" "$NAME" | tail -2
