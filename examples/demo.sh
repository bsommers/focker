#!/usr/bin/env bash
# End-to-end walkthrough: run -> cutover -> handover, narrated.
#   make demo        (or examples/demo.sh after 'make')
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/build
F=$B/focker
export FOCKER_RUNTIME_DIR=${FOCKER_RUNTIME_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/focker-demo.XXXXXX")}
NAME=meet_the_parents

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
show() { printf '$ %s\n' "$*"; "$@"; }

cleanup() { "$F" stop "$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT

say "1. Start a family. focker is the subreaper; oldparent starts two tickers and relays their output over pipes"
show "$F" run -d -n "$NAME" -- "$B/oldparent" 2 -- -i 300
sleep 1
show "$F" tree "$NAME"

SUP=$(cat "$FOCKER_RUNTIME_DIR/$NAME/pid")
OLD=$(pgrep -P "$SUP" | head -1)
T=$(pgrep -P "$OLD" | sort -n | head -1)

say "2. Dry run: what would adopting ticker $T involve?"
show "$F" cutover --dry-run "$NAME" "$T"

say "3. Cut over: freeze $T, take over oldparent's pipe, kill oldparent, verify, thaw"
show "$F" cutover "$NAME" "$T"
sleep 1
show "$F" tree "$NAME"
echo "(the other ticker had no one holding its pipe, so it died of SIGPIPE)"
show "$F" inspect "$NAME"

say "4. ticker $T keeps running: focker drains its stdout to a log"
show tail -3 "$FOCKER_RUNTIME_DIR/$NAME/adopted-$T-fd1.log"

say "5. Hand over: focker execs newparent in place. Same PID, so $T is now newparent's real child"
show "$F" handover "$NAME" -- "$B/newparent"
sleep 1.2
printf '$ ps -o pid,ppid,cmd -p %s,%s\n' "$SUP" "$T"
ps -o pid,ppid,cmd -p "$SUP","$T"
show tail -4 "$FOCKER_RUNTIME_DIR/$NAME/focker.log"

say "6. Done. Stopping ticker $T; newparent reaps it and exits"
kill "$T"
sleep 0.5
show tail -2 "$FOCKER_RUNTIME_DIR/$NAME/focker.log"
show "$F" ps -a
