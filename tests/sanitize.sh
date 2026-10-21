#!/usr/bin/env bash
# Run the unit and integration suites against a sanitizer build (make asan)
# and fail if any process wrote an ASan, LSan or UBSan report.
#   tests/sanitize.sh [BUILD_DIR]     (default: build-asan)
# Reports go to files, not stderr: a detached supervisor's stderr is its
# focker.log, which the suite deletes with the runtime dir.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-build-asan}
case $B in /*) ;; *) B=$ROOT/$B ;; esac
REPORTS=$(mktemp -d "${TMPDIR:-/tmp}/focker-san.XXXXXX")
trap 'rm -rf "$REPORTS"' EXIT

export ASAN_OPTIONS="log_path=$REPORTS/asan:detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="log_path=$REPORTS/ubsan:print_stacktrace=1:halt_on_error=1"

rc=0
"$B/unit" || rc=1
FOCKER_BUILD=$B "$ROOT/tests/run.sh" || rc=1

shopt -s nullglob
reports=("$REPORTS"/*)
if [ ${#reports[@]} -gt 0 ]; then
    echo
    echo "${#reports[@]} sanitizer report(s):"
    for f in "${reports[@]}"; do
        echo "==> $(basename "$f") <=="
        head -40 "$f"
    done
    rc=1
else
    echo "no sanitizer reports"
fi
exit $rc
