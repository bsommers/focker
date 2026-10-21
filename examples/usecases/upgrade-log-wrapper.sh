#!/usr/bin/env bash
# Use case: a wrapper (log shipper, job runner, batch driver) sits in front of
# a long job, and the wrapper itself needs replacing: a bug fix, new config,
# a memory leak. Restarting the wrapper normally means restarting the job.
# Here v1 is retired and v2 takes the running job over as its real child,
# continuing the same output stream without losing a line.
set -eu
# shellcheck source=examples/usecases/lib.sh
. "$(dirname "$0")/lib.sh"
need_python

NAME=shipper
FAMILIES+=("$NAME")
OUT=$FOCKER_RUNTIME_DIR/shipped.log
say "logwrap v1 runs a job and ships its output to $OUT"
show "$F" run -d -n "$NAME" -- "$UC/logwrap.py" v1 "$OUT" -- "$B/ticker" -i 100
wait_for test -s "$OUT"
SUP=$(sup_of "$NAME")
V1=$(kids_of "$SUP" | head -1)
JOB=$(kids_of "$V1" | head -1)
show tail -2 "$OUT"

say "Retire v1: focker adopts the job and drains its pipe in the meantime"
show "$F" cutover --from "$V1" "$NAME"
sleep 0.5

say "Hand the job to v2 (focker execs it in place; the job becomes its real child)"
show "$F" handover "$NAME" -- "$UC/logwrap.py" v2 "$OUT"
wait_for bash -c "grep -q '^\[v2\]' '$OUT'" || die "v2 is not shipping"
[ "$(ps -o ppid= -p "$JOB" | tr -d ' ')" = "$SUP" ] || die "the job's parent is not v2"
ok "the job ($JOB) is a child of v2, which kept the supervisor's PID ($SUP)"

show grep -m3 -E '^\[v1\]|drained|^\[v2\]' "$OUT"
grep -o 'tick [0-9]*' "$OUT" | awk '{print $2}' | awk 'NR > 1 && $1 != prev + 1 { gap = 1; print "gap after tick " prev } { prev = $1 } END { exit gap }' ||
    die "lines were lost across the upgrade"
ok "tick numbers are continuous across v1 -> focker -> v2: no line lost"

say "v2 reaps the job like any parent would"
kill "$JOB"
wait_for bash -c "! kill -0 $JOB 2>/dev/null" || die "the job was not reaped"
show tail -1 "$FOCKER_RUNTIME_DIR/$NAME/focker.log"
