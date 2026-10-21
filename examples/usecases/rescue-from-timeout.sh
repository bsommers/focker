#!/usr/bin/env bash
# Use case: a job was started under a deadline (timeout(1), a cron wrapper, a
# CI step limit) and it needs longer than planned. Kill the deadline, keep the
# job, and still learn its exit status.
#
# timeout(1) traps SIGTERM, passes it to the job and waits for the job to
# exit, so a frozen job would never let it die: cut it off with SIGKILL.
set -eu
# shellcheck source=examples/usecases/lib.sh
. "$(dirname "$0")/lib.sh"

NAME=deadline
FAMILIES+=("$NAME")
say "A 6-step job is running under 'timeout 3'; left alone it dies at step 3"
# shellcheck disable=SC2016  # the job's own script
show "$F" run -d --linger -n "$NAME" -- timeout 3 sh -c 'for i in 1 2 3 4 5 6; do echo "step $i"; sleep 1; done; exit 3'
sleep 0.5
show "$F" tree "$NAME"
TO=$(kids_of "$(sup_of "$NAME")" | head -1)
JOB=$(kids_of "$TO" | head -1)

say "Retire the deadline: adopt the job, SIGKILL timeout(1)"
show "$F" cutover -s KILL --from "$TO" "$NAME"

say "The job runs past the 3s deadline, and focker reaps it and keeps its exit status"
wait_for bash -c "'$F' inspect $NAME | grep -Eq '^ +$JOB +adopted .*exited'" || die "job $JOB did not finish under focker"
show "$F" inspect "$NAME"
"$F" logs "$NAME" | grep -q 'step 6' || die "the job didn't reach step 6"
ok "the job reached step 6, three steps past its deadline"
"$F" inspect "$NAME" | grep -Eq "^ +$JOB +adopted +$TO +exited 3 " || die "exit status was not recorded"
ok "its exit status (3) is recorded; under init it would have been lost"
