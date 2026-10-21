#!/usr/bin/env bash
# Measure what an idle focker supervisor costs: CPU time and wakeups per
# second while its only child sleeps.
#   tests/idle-cost.sh [SECONDS]     (default 20; uses FOCKER_BUILD, default build)
# CPU comes from /proc/PID/schedstat (ns on CPU), not utime, which counts in
# 10 ms ticks and rounds an idle supervisor down to 0.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${FOCKER_BUILD:-build}
case $B in /*) ;; *) B=$ROOT/$B ;; esac
SECS=${1:-20}
FOCKER_RUNTIME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/focker-idle.XXXXXX") || exit 1
export FOCKER_RUNTIME_DIR
trap '"$B/focker" stop idle >/dev/null 2>&1; sleep 0.3; rm -rf "$FOCKER_RUNTIME_DIR"' EXIT

"$B/focker" run -d -n idle -- sleep 600 >/dev/null || exit 1
SUP=$(cat "$FOCKER_RUNTIME_DIR/idle/pid")
sleep 1

sample() { # -> "ns switches"
    local ns sw
    read -r ns _ < "/proc/$SUP/schedstat"
    sw=$(awk '/ctxt_switches/ {s += $2} END {print s}' "/proc/$SUP/status")
    echo "$ns $sw"
}
read -r ns0 sw0 < <(sample)
sleep "$SECS"
read -r ns1 sw1 < <(sample)

nprocs=$(find /proc -maxdepth 1 -name '[0-9]*' | wc -l)
awk -v ns=$((ns1 - ns0)) -v sw=$((sw1 - sw0)) -v s="$SECS" -v p="$nprocs" 'BEGIN {
    printf "idle supervisor over %ds: CPU %.1f us/s, %.2f wakeups/s (%d processes on the system)\n",
           s, ns / s / 1000, sw / s, p
}'
