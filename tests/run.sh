#!/usr/bin/env bash
# Integration tests for focker. Run from anywhere: tests/run.sh
# Each test starts real processes in a private runtime dir and checks
# exit codes, ppids and output.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/build
F=$B/focker
export FOCKER_RUNTIME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/focker-test.XXXXXX")
unset FOCKER_FREEZE

pass=0 fail=0
failed=()

cleanup() {
    for d in "$FOCKER_RUNTIME_DIR"/*/; do
        [ -f "$d/pid" ] || continue
        pid=$(cat "$d/pid")
        pkill -KILL -P "$pid" 2>/dev/null
        kill -KILL "$pid" 2>/dev/null
    done
    rm -rf "$FOCKER_RUNTIME_DIR"
}
trap cleanup EXIT

ok()   { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail + 1)); failed+=("$1"); printf '  FAIL  %s\n' "$1"; [ -n "${2:-}" ] && printf '        %s\n' "$2"; }
check() { local name=$1; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
expect_rc() { # name want cmd...
    local name=$1 want=$2; shift 2
    local out rc
    out=$("$@" 2>&1); rc=$?
    if [ "$rc" -eq "$want" ]; then ok "$name"; else bad "$name" "exit $rc, wanted $want: $(echo "$out" | head -3)"; fi
}

sup_of()   { cat "$FOCKER_RUNTIME_DIR/$1/pid"; }
kids_of()  { pgrep -P "$1" | sort -n; }
ppid_of()  { ps -o ppid= -p "$1" 2>/dev/null | tr -d ' '; }
alive()    { [ -n "$(ps -o pid= -p "$1" 2>/dev/null)" ] && [ "$(ps -o stat= -p "$1" | cut -c1)" != Z ]; }
wait_for() { # up to 3s for a command to succeed
    for _ in $(seq 60); do "$@" && return 0; sleep 0.05; done; return 1
}
start() { # name cmd...  -> starts detached, waits for grandchildren
    local name=$1; shift
    "$F" run -d -n "$name" -- "$@" >/dev/null || return 1
    local sup; sup=$(sup_of "$name")
    wait_for test -n "$(kids_of "$sup")" && wait_for test -n "$(kids_of "$(kids_of "$sup" | head -1)")"
}
old_of()    { kids_of "$(sup_of "$1")" | head -1; }
ticker_of() { kids_of "$(old_of "$1")" | head -1; }
stop_family() { "$F" stop "$1" >/dev/null 2>&1; wait_for test ! -S "$FOCKER_RUNTIME_DIR/$1/control.sock"; }

echo "focker tests (runtime dir $FOCKER_RUNTIME_DIR)"

echo "cli"
expect_rc "no arguments is a usage error"         2 "$F"
expect_rc "unknown command is a usage error"      2 "$F" frobnicate
expect_rc "--help succeeds"                       0 "$F" --help
expect_rc "version succeeds"                      0 "$F" version
expect_rc "cutover --help succeeds"               0 "$F" cutover --help
expect_rc "run without a command"                 2 "$F" run -n x
expect_rc "bad family name"                       2 "$F" run -n '../evil' -- true
expect_rc "bad freeze method"                     2 "$F" cutover -f teleport fam 1
expect_rc "bad signal name"                       2 "$F" cutover -s SIGNOPE fam 1
expect_rc "bad timeout"                           2 "$F" cutover -t 0 fam 1
expect_rc "bad pid"                               2 "$F" cutover fam abc
expect_rc "cutover needs a pid"                   2 "$F" cutover fam
expect_rc "missing family is a runtime error"     1 "$F" cutover nobody 1
expect_rc "inspect missing family"                1 "$F" inspect nobody
expect_rc "bad FOCKER_FREEZE"                     2 env FOCKER_FREEZE=nope "$F" cutover fam 1
expect_rc "ps on empty runtime dir"               0 "$F" ps

echo "run"
expect_rc "foreground run returns the command's status" 3 "$F" run -n fg -- sh -c 'exit 3'
check "foreground family leaves an exit record" grep -q "exited 3" "$FOCKER_RUNTIME_DIR/fg/exit"
expect_rc "rm exited family"                      0 "$F" rm fg
expect_rc "detached run of a missing program fails" 1 "$F" run -d -n missing -- /nonexistent/prog
check "failed start frees the name" test ! -e "$FOCKER_RUNTIME_DIR/missing"
expect_rc "foreground run of a missing program" 127 "$F" run -n missing2 -- /nonexistent/prog
"$F" run -d -n dup -- sleep 30 >/dev/null
expect_rc "duplicate name is refused"             1 "$F" run -d -n dup -- true
expect_rc "rm refuses a running family"           1 "$F" rm dup
stop_family dup; "$F" rm dup >/dev/null

echo "cutover (pipe, ptrace)"
start pipe "$B/oldparent" 2 -- -i 50
SUP=$(sup_of pipe) OLD=$(old_of pipe) T=$(ticker_of pipe)
OTHER=$(kids_of "$OLD" | tail -1)
out=$("$F" cutover --dry-run pipe "$T"); rc=$?
check "dry run succeeds" test $rc -eq 0
check "dry run plans the pipe takeover" grep -q "plan: take over pipe-r" <<<"$out"
check "dry run names the bystander" grep -q "plan: $OTHER" <<<"$out"
check "dry run changes nothing" test "$(ppid_of "$T")" = "$OLD"
expect_rc "cutover succeeds"                      0 "$F" cutover pipe "$T"
check "ticker's parent is now focker" test "$(ppid_of "$T")" = "$SUP"
check "old parent is gone" wait_for bash -c "! kill -0 $OLD 2>/dev/null || ps -o stat= -p $OLD | grep -q Z"
sleep 0.3
check "ticker is still running" alive "$T"
check "ticker is not left stopped" test "$(ps -o stat= -p "$T" | cut -c1)" != T
check "ticker output after cutover is captured" wait_for grep -q "ppid=$SUP" "$FOCKER_RUNTIME_DIR/pipe/adopted-$T-fd1.log"
check "bystander was adopted too (then died of SIGPIPE)" grep -q "$OTHER .*adopted" <<<"$("$F" inspect pipe)"
check "tree marks the adopted child" grep -q "$T .*(adopted from $OLD)" <<<"$("$F" tree pipe)"
expect_rc "logs -p shows captured output"         0 "$F" logs -p "$T" pipe
expect_rc "cutover of a direct child is a no-op"  0 "$F" cutover pipe "$T"

echo "handover"
out=$("$F" handover pipe -- "$B/newparent"); rc=$?
check "handover succeeds" test $rc -eq 0
check "supervisor pid kept, program replaced" wait_for grep -q newparent "/proc/$SUP/cmdline"
check "ticker is a child of the new parent" test "$(ppid_of "$T")" = "$SUP"
check "new parent got FOCKER_FDS" wait_for grep -q "FOCKER_FDS=[0-9]*:$T:pipe-r:1" "$FOCKER_RUNTIME_DIR/pipe/focker.log"
check "new parent relays the inherited pipe" wait_for grep -q "\[newparent $SUP\] tick" "$FOCKER_RUNTIME_DIR/pipe/focker.log"
kill "$T"
check "new parent reaps the adopted child" wait_for grep -q "reaped $T" "$FOCKER_RUNTIME_DIR/pipe/focker.log"
check "ps -a reports the handover" grep -q "handed over" <<<"$("$F" ps -a)"

echo "cutover (socketpair, threads)"
start sock "$B/oldparent" -s 1 -- -i 50 -t 4
SUP=$(sup_of sock) T=$(ticker_of sock)
out=$("$F" cutover sock "$T"); rc=$?
check "cutover over a socketpair succeeds" test $rc -eq 0
check "all 5 threads were frozen" grep -q "5 threads" <<<"$out"
check "unix socket peer was taken over" grep -q "took over unix" <<<"$out"
sleep 0.3
check "ticker survives on the socket" alive "$T"
check "socket output is captured" wait_for grep -q "ppid=$SUP" "$FOCKER_RUNTIME_DIR/sock/adopted-$T-fd1.log"
stop_family sock

echo "signals during the cutover"
start pd "$B/oldparent" 1 -- -i 50 -d 1
T=$(ticker_of pd)
out=$("$F" cutover pd "$T")
check "ptrace freeze swallows PR_SET_PDEATHSIG" grep -q "swallowed SIGHUP" <<<"$out"
sleep 0.2; check "child with pdeathsig survives" alive "$T"
stop_family pd

start pds "$B/oldparent" 1 -- -i 50 -d 1
T=$(ticker_of pds)
out=$("$F" cutover -f signal pds "$T")
check "signal freeze warns about pdeathsig" grep -q "will be delivered" <<<"$out"
check "child with pdeathsig dies under signal freeze" wait_for bash -c "! kill -0 $T 2>/dev/null"
stop_family pds

start killer sh -c "$B/ticker -i 50 >/dev/null & trap 'kill \$!; exit' TERM; wait"
T=$(ticker_of killer)
out=$("$F" cutover killer "$T")
check "parent's dying 'kill my child' is swallowed" grep -q "swallowed SIGTERM" <<<"$out"
sleep 0.2; check "child survives a parent that kills it on exit" alive "$T"
stop_family killer

echo "failure paths"
start stubborn sh -c "trap '' TERM; $B/ticker -i 50 >/dev/null & wait; wait"
OLD=$(old_of stubborn) T=$(ticker_of stubborn)
expect_rc "timeout when the old parent won't die" 1 "$F" cutover -t 0.3 stubborn "$T"
check "rollback leaves the child with its old parent" test "$(ppid_of "$T")" = "$OLD"
check "rollback leaves the child running" test "$(ps -o stat= -p "$T" | cut -c1)" != T
expect_rc "KILL gets past a TERM-ignoring parent" 0 "$F" cutover -s KILL stubborn "$T"
expect_rc "a pid outside the family is refused"   1 "$F" cutover stubborn $$
expect_rc "focker itself is refused"              1 "$F" cutover stubborn "$(sup_of stubborn)"
stop_family stubborn

start nested sh -c "sh -c '$B/ticker -i 50 >/dev/null & wait' & wait"
MID=$(ticker_of nested)
T=$(kids_of "$MID" | head -1)
expect_rc "a target can't also be the parent of another target" 1 "$F" cutover nested "$MID" "$T"
expect_rc "grandchildren are cut over from their own parent" 0 "$F" cutover nested "$T"
check "grandchild skipped a generation to focker" test "$(ppid_of "$T")" = "$(sup_of nested)"
stop_family nested

start from "$B/oldparent" 3 -- -i 50
OLD=$(old_of from)
out=$("$F" cutover --from "$OLD" from); rc=$?
check "--from adopts every child" test $rc -eq 0 -a "$(grep -c '^cutover: adopted' <<<"$out")" -eq 3
check "--from keeps all three alive" test "$(kids_of "$(sup_of from)" | wc -l)" -eq 3
expect_rc "freeze a member"                       0 "$F" freeze from "$(kids_of "$(sup_of from)" | head -1)"
expect_rc "thaw a member"                         0 "$F" thaw from "$(kids_of "$(sup_of from)" | head -1)"
expect_rc "freeze refuses outsiders"              1 "$F" freeze from $$
stop_family from
check "family exits once empty" test ! -S "$FOCKER_RUNTIME_DIR/from/control.sock"

echo "cutover edge cases"
start_linger() { # like start, but the family outlives its last child so inspect still answers
    local name=$1; shift
    "$F" run -d --linger -n "$name" -- "$@" >/dev/null || return 1
    local sup; sup=$(sup_of "$name")
    wait_for test -n "$(kids_of "$sup")" && wait_for test -n "$(kids_of "$(kids_of "$sup" | head -1)")"
}
# REL-01: URG (23) is passed through before VTALRM (26) is swallowed, and -x 23 makes it fatal,
# so the ticker exits inside the swallow loop while still traced.
start_linger dies sh -c "$B/ticker -i 50 -d 26 -x 23 >/dev/null & trap 'kill -URG \$!; exit' TERM; wait"
T=$(ticker_of dies)
"$F" cutover dies "$T" >/dev/null 2>&1
check "a target that dies during thaw keeps its exit status" \
    wait_for bash -c "'$F' inspect dies | grep -q '^  $T .*exited 7'"
stop_family dies

# REL-02: the old parent stops its child on the way out; the stop must survive the thaw.
# The pdeathsig (26) makes the swallow loop run, so the group-stop is seen under trace.
start stopme sh -c "$B/ticker -i 50 -d 26 >/dev/null & trap 'kill -STOP \$!; exit' TERM; wait"
T=$(ticker_of stopme)
out=$("$F" cutover stopme "$T")
check "the swallow loop ran" grep -q "swallowed SIGVTALRM" <<<"$out"
check "a SIGSTOP sent during the cutover is kept" grep -q "SIGSTOP reached $T during the cutover; it was kept" <<<"$out"
check "the target stays stopped after the thaw" wait_for bash -c "test \"\$(ps -o stat= -p $T | cut -c1)\" = T"
kill -CONT "$T"
check "the stopped target resumes on SIGCONT" \
    wait_for bash -c "s=\$(ps -o stat= -p $T | cut -c1); test -n \"\$s\" -a \"\$s\" != T -a \"\$s\" != Z"
stop_family stopme

# REL-03: the target dies between validation and freeze (held open by the test-only stall)
FOCKER_TEST_STALL_MS=600 start pin sh -c "$B/ticker -i 50 >/dev/null & wait; sleep 30"
OLD=$(old_of pin) T=$(ticker_of pin)
"$F" cutover pin "$T" >"$FOCKER_RUNTIME_DIR/pin.out" 2>&1 & cpid=$!
sleep 0.2; kill -KILL "$T"
wait "$cpid"; rc=$?
check "a target that dies before the freeze fails the cutover" test $rc -eq 1
check "the pidfd notices the target died" grep -q "$T exited before it could be frozen" "$FOCKER_RUNTIME_DIR/pin.out"
check "the old parent is left alone" alive "$OLD"
stop_family pin

# REL-04: SIGUSR1 is blocked and pending in the ticker, and the old parent sends another on exit
start pend sh -c "$B/ticker -i 50 -b 10 >/dev/null & trap 'kill -USR1 \$!; exit' TERM; wait"
T=$(ticker_of pend)
out=$("$F" cutover pend "$T")
check "an already-pending signal is reported" grep -q "SIGUSR1 was already pending for $T" <<<"$out"
check "the target with a pending signal survives" alive "$T"
stop_family pend

echo
echo "$pass passed, $fail failed"
for t in "${failed[@]}"; do echo "  failed: $t"; done
[ "$fail" -eq 0 ]
