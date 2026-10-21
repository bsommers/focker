#!/usr/bin/env bash
# Use case: take a consistent copy (backup, LVM/ZFS/btrfs snapshot, rsync) of
# files a running job keeps writing. Pause the writer, copy, resume. The job
# isn't restarted and never sees an error.
set -eu
# shellcheck source=examples/usecases/lib.sh
. "$(dirname "$0")/lib.sh"

NAME=writer
FAMILIES+=("$NAME")
DATA=$FOCKER_RUNTIME_DIR/data.txt
say "A job appends a record every 20 ms to $DATA"
show "$F" run -d -n "$NAME" -- sh -c "i=0; while :; do i=\$((i+1)); echo \"record \$i\" >> '$DATA'; sleep 0.02; done"
wait_for test -s "$DATA"
JOB=$(kids_of "$(sup_of "$NAME")" | head -1)

say "Pause it, copy, and check nothing moved while paused"
show "$F" freeze "$NAME" "$JOB"
s1=$(stat -c %s "$DATA")
cp "$DATA" "$DATA.snapshot"   # stand-in for 'lvcreate -s', 'zfs snapshot', 'rsync'
sleep 0.5
s2=$(stat -c %s "$DATA")
[ "$s1" = "$s2" ] || die "the file changed while the job was paused"
cmp -s "$DATA" "$DATA.snapshot" || die "the copy differs from the paused file"
ok "the file was stable for the whole copy ($s1 bytes)"

show "$F" thaw "$NAME" "$JOB"
wait_for bash -c "[ \$(stat -c %s '$DATA') -gt $s2 ]" || die "the job did not resume"
ok "the job resumed where it left off: $(tail -1 "$DATA")"
