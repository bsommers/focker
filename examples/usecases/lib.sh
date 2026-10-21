# Shared helpers for the use-case walkthroughs. Each walkthrough is also a
# check: it exits non-zero if what it claims didn't happen.
# shellcheck shell=bash

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
B=${FOCKER_BUILD:-$ROOT/build}
F=$B/focker
# shellcheck disable=SC2034  # used by the walkthroughs
UC=$ROOT/examples/usecases
export FOCKER_RUNTIME_DIR=${FOCKER_RUNTIME_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/focker-usecase.XXXXXX")}

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
show() { printf '$ %s\n' "$*"; "$@"; }
ok()   { printf '\033[32mok\033[0m    %s\n' "$*"; }
die()  { printf '\033[31mFAIL\033[0m  %s\n' "$*"; exit 1; }

sup_of()  { cat "$FOCKER_RUNTIME_DIR/$1/pid"; }
kids_of() { pgrep -P "$1" | sort -n; }
wait_for() { # up to 10s for a command to succeed
    for _ in $(seq 100); do "$@" && return 0; sleep 0.1; done
    return 1
}

need_python() {
    command -v python3 >/dev/null || { echo "skip: needs python3"; exit 0; }
}

FAMILIES=()
cleanup() {
    local n
    for n in "${FAMILIES[@]}"; do
        "$F" stop -s KILL "$n" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT
