#!/usr/bin/env bash
# Integration tests for focker. Run from anywhere: tests/run.sh
# Each test starts real processes in a private runtime dir and checks
# exit codes, ppids and output.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${FOCKER_BUILD:-build}
case $B in /*) ;; *) B=$ROOT/$B ;; esac
F=$B/focker
FOCKER_RUNTIME_DIR=$(mktemp -d "${TMPDIR:-/tmp}/focker-test.XXXXXX") || exit 1
export FOCKER_RUNTIME_DIR
unset FOCKER_FREEZE

pass=0 fail=0
failed=()

kill_family() { # DIR: the family's state dir
    local pid
    pid=$(cat "$1/pid" 2>/dev/null) || return 0
    pkill -KILL -P "$pid" 2>/dev/null
    # a detached supervisor leads its own session, so this also reaches
    # children that a crashed supervisor left to some other reaper
    pkill -KILL -s "$pid" 2>/dev/null
    kill -KILL "$pid" 2>/dev/null
}
cleanup() {
    for d in "$FOCKER_RUNTIME_DIR"/*/; do
        kill_family "$d"
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
has_grandchild() { local k; k=$(kids_of "$1" | head -1); [ -n "$k" ] && [ -n "$(kids_of "$k")" ]; }
start() { # [--linger] name cmd...  -> starts detached, waits for grandchildren
    local opt=()
    [ "$1" = --linger ] && { opt=(--linger); shift; }
    local name=$1; shift
    "$F" run -d "${opt[@]}" -n "$name" -- "$@" >/dev/null || return 1
    wait_for has_grandchild "$(sup_of "$name")"
}
lacks() { ! grep -q -- "$1" <<<"$2"; }   # pattern text
nkids() { kids_of "$1" | wc -l; }
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

echo "cutover (tcp)"
tcp_family() { # NAME FLAG ADDR-REGEX
    local name=$1 flag=$2 addr=$3 OLD T out rc
    if ! start "$name" "$B/oldparent" "$flag" 2 -- -i 50; then
        bad "$name: the tcp family starts"; return
    fi
    OLD=$(old_of "$name") T=$(ticker_of "$name")
    out=$("$F" cutover --dry-run --from "$OLD" "$name" 2>&1)
    check "$name: dry run plans the tcp takeover" \
        grep -Eq "plan: take over tcp fd [0-9]+ of $OLD \(child $T fd 1\).*\($addr:[0-9]+ <-> $addr:[0-9]+\)" <<<"$out"
    out=$("$F" cutover --from "$OLD" "$name" 2>&1); rc=$?
    check "$name: tcp cutover succeeds" test $rc -eq 0
    check "$name: both connections were taken over" test "$(grep -c 'took over tcp' <<<"$out")" -eq 2
    check "$name: the tcp child keeps writing after the cutover" \
        wait_for grep -q "ppid=$(sup_of "$name")" "$FOCKER_RUNTIME_DIR/$name/adopted-$T-fd1.log"
    check "$name: the tcp child is alive" alive "$T"
    check "$name: inspect lists a tcp fd" grep -Eq '^ *[0-9]+ +tcp ' <<<"$("$F" inspect "$name")"
    stop_family "$name"
}
tcp_family tcp4 -T4 '127\.0\.0\.1'
if python3 -c 'import socket; s=socket.socket(socket.AF_INET6); s.bind(("::1",0))' 2>/dev/null; then
    tcp_family tcp6 -T6 '\[::1\]'
else
    printf '  skip  tcp6 (cannot bind ::1)\n'
fi
if command -v python3 >/dev/null; then
    start tlis python3 -c '
import socket, subprocess, sys
l = socket.socket(); l.bind(("127.0.0.1", 0)); l.listen()
subprocess.Popen([sys.argv[1], "-i", "50"], pass_fds=[l.fileno()], stdout=subprocess.DEVNULL).wait()' "$B/ticker"
    T=$(ticker_of tlis)
    out=$("$F" cutover --dry-run tlis "$T" 2>&1)
    check "a shared listening socket is not taken" lacks "tcp" "$out"
    stop_family tlis
else
    printf '  skip  listening socket (needs python3)\n'
fi

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
# REL-01: URG (23) is passed through before VTALRM (26) is swallowed, and -x 23 makes it fatal,
# so the ticker exits inside the swallow loop while still traced.
start --linger dies sh -c "$B/ticker -i 50 -d 26 -x 23 >/dev/null & trap 'kill -URG \$!; exit' TERM; wait"
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

echo "cutover options"
start nodrain "$B/oldparent" 1 -- -i 50
T=$(ticker_of nodrain)
out=$("$F" cutover --no-drain nodrain "$T")
check "--no-drain holds the pipe open" grep -q "took over pipe-r .*held open" <<<"$out"
check "--no-drain writes no capture file" test ! -e "$FOCKER_RUNTIME_DIR/nodrain/adopted-$T-fd1.log"
check "inspect lists the held fd without a log" grep -Eq "pipe-r +$T +1 +[0-9]+ +-$" <<<"$("$F" inspect nodrain)"
check "--no-drain keeps the child alive" alive "$T"
stop_family nodrain

start nosup "$B/oldparent" 1 -- -i 50 -d 1
T=$(ticker_of nosup)
out=$("$F" cutover --no-suppress nosup "$T")
check "--no-suppress swallows nothing" lacks swallowed "$out"
check "--no-suppress delivers the pdeathsig" wait_for bash -c "! kill -0 $T 2>/dev/null"
stop_family nosup

start waiter sh -c "$B/ticker -i 50 >/dev/null & exec sleep 1"
SUP=$(sup_of waiter) T=$(ticker_of waiter)
out=$("$F" cutover --wait -t 5 waiter "$T"); rc=$?
check "--wait succeeds once the parent exits on its own" test $rc -eq 0
check "--wait doesn't signal the parent" grep -q "waiting for old parent" <<<"$out"
check "--wait adopts the child" test "$(ppid_of "$T")" = "$SUP"
stop_family waiter

echo "async cutover"
async_fam() { # NAME [OLDPARENT-FLAGS [TICKER-FLAGS]]: two oldparents P1, P2 under one sh; T1 (P1's ticker) is adopted with its pipe draining
    start "$1" sh -c "$B/oldparent ${2:-} 1 -- -i 50 ${3:-} & $B/oldparent ${2:-} 1 -- -i 50 ${3:-} & wait" || return 1
    SUP=$(sup_of "$1")
    local sh; sh=$(kids_of "$SUP" | head -1)
    wait_for bash -c "[ \$(pgrep -P $sh | wc -l) -eq 2 ] && for p in \$(pgrep -P $sh); do pgrep -P \$p >/dev/null || exit 1; done"
    read -r P1 P2 <<<"$(kids_of "$sh" | tr '\n' ' ')"
    T1=$(kids_of "$P1" | head -1) T2=$(kids_of "$P2" | head -1)
    "$F" cutover --from "$P1" "$1" >/dev/null
}
frozen_by() { grep -q "^TracerPid:[[:space:]]*$2\$" "/proc/$1/status" 2>/dev/null; }   # PID SUP
busy() { local out; out=$("$@" 2>&1) && return 1; grep -q 'busy (a cutover is in progress)' <<<"$out"; }

async_fam async
"$F" cutover --wait -t 5 async "$T2" >"$FOCKER_RUNTIME_DIR/async.out" 2>&1 & cpid=$!
wait_for frozen_by "$T2" "$SUP"
log1=$FOCKER_RUNTIME_DIR/async/adopted-$T1-fd1.log
s1=$(stat -c %s "$log1"); sleep 0.5; s2=$(stat -c %s "$log1")
check "drains keep running during a cutover" test "$s2" -gt "$s1"
expect_rc "inspect answers during a cutover"      0 timeout 2 "$F" inspect async
check "info answers during a cutover" bash -c "timeout 2 '$F' ps | grep -qw async"
check "a second cutover is busy" busy "$F" cutover async "$T1"
check "a dry run is busy too" busy "$F" cutover --dry-run async "$T1"
check "release is busy" busy "$F" release async 3
check "handover is busy" busy "$F" handover async -- /nonexistent/parent
check "stop is busy" busy "$F" stop async
check "stop leaves the family running" alive "$T1"
kill "$P2"
wait "$cpid"; rc=$?
check "the waiting cutover completes" test $rc -eq 0
check "the waiting cutover adopts the child" grep -q "adopted $T2" "$FOCKER_RUNTIME_DIR/async.out"
stop_family async

async_fam async2
"$F" cutover --wait -t 5 async2 "$T2" >/dev/null 2>&1 & cpid=$!
wait_for frozen_by "$T2" "$SUP"
kill -9 "$cpid"; wait "$cpid" 2>/dev/null
kill "$P2"
check "a vanished client doesn't stop the cutover" wait_for bash -c "[ \"\$(ps -o ppid= -p $T2 | tr -d ' ')\" = $SUP ]"
expect_rc "the supervisor survives a vanished client" 0 "$F" inspect async2
stop_family async2

async_fam async3 -I "-t 4"
"$F" cutover --wait -t 1 async3 "$T2" >/dev/null 2>&1 & cpid=$!
wait_for frozen_by "$T2" "$SUP"
kill -9 "$T2"
wait "$cpid"; rc=$?
check "a target killed during the wait fails the cutover" test $rc -eq 1
out=$("$F" inspect async3)
check "a target killed during the wait is not a member" lacks "^ \+$T2 " "$out"
check "nor are its threads" lacks "orphan .*killed by" "$out"
check "its held stdin is closed" bash -c "! grep -Eq '^ +[0-9]+ +pipe-w +$T2 ' <<<'$out'"
kill_family "$FOCKER_RUNTIME_DIR/async3"

async_fam async4
"$F" cutover --wait -t 5 async4 "$T2" >"$FOCKER_RUNTIME_DIR/async4.out" 2>&1 & cpid=$!
wait_for frozen_by "$T2" "$SUP"
kill -TERM "$SUP"
check "SIGTERM during the wait ends the cutover" wait_for bash -c "! kill -0 $cpid 2>/dev/null"
wait "$cpid"; rc=$?
check "SIGTERM during the wait fails the cutover" test $rc -eq 1
check "SIGTERM during the wait rolls back first" \
    bash -c "grep -q 'cancelled by SIGTERM' '$FOCKER_RUNTIME_DIR/async4.out' && grep -q 'rolled back' '$FOCKER_RUNTIME_DIR/async4.out'"
check "T2 is not left stopped" bash -c "! kill -0 $T2 2>/dev/null ||
    { ps -o stat= -p $T2 | grep -qv '^[Tt]' && grep -q '^TracerPid:[[:space:]]*0\$' /proc/$T2/status; }"
kill -9 "$T2" 2>/dev/null
kill_family "$FOCKER_RUNTIME_DIR/async4"

echo "logs"
start logf "$B/oldparent" 1 -- -i 50
"$F" logs -f logf >"$FOCKER_RUNTIME_DIR/logf.out" 2>&1 & lpid=$!
sleep 0.3
stop_family logf
check "logs -f exits when the family stops" wait_for bash -c "! kill -0 $lpid 2>/dev/null"
check "logs -f printed the log" grep -q "started" "$FOCKER_RUNTIME_DIR/logf.out"
kill "$lpid" 2>/dev/null; wait "$lpid" 2>/dev/null

echo "handover failure"
start hof "$B/oldparent" 1 -- -i 50
T=$(ticker_of hof)
"$F" cutover hof "$T" >/dev/null
printf '#!/nonexistent/interpreter\n' >"$FOCKER_RUNTIME_DIR/badparent"; chmod +x "$FOCKER_RUNTIME_DIR/badparent"
out=$("$F" handover hof -- "$FOCKER_RUNTIME_DIR/badparent" 2>&1); rc=$?
check "a failed handover exits 1" test $rc -eq 1
check "a failed handover says why" grep -q "cannot exec .*badparent" <<<"$out"
check "a failed handover is logged" wait_for grep -q "handover to .* failed" "$FOCKER_RUNTIME_DIR/hof/focker.log"
expect_rc "the family still answers after a failed handover" 0 "$F" inspect hof
check "ps -a doesn't report a handover" lacks "handed over" "$("$F" ps -a | grep hof)"
LOG=$FOCKER_RUNTIME_DIR/hof/adopted-$T-fd1.log
n1=$(wc -l <"$LOG")
check "held fds still drain after a failed handover" wait_for bash -c "test \$(wc -l <'$LOG') -gt $n1"
stop_family hof

echo "long runtime path"
LONG=$FOCKER_RUNTIME_DIR/$(printf 'd%.0s' $(seq 120))
mkdir -m 700 "$LONG"
FOCKER_RUNTIME_DIR=$LONG start longp "$B/oldparent" 1 -- -i 50
expect_rc "a runtime path past sun_path's 108 bytes still answers" 0 env FOCKER_RUNTIME_DIR="$LONG" "$F" inspect longp
T=$(FOCKER_RUNTIME_DIR=$LONG ticker_of longp)
expect_rc "cutover works over the /proc/self/fd socket path" 0 env FOCKER_RUNTIME_DIR="$LONG" "$F" cutover longp "$T"
FOCKER_RUNTIME_DIR=$LONG stop_family longp
check "the long-path family shut down" test ! -S "$LONG/longp/control.sock"

echo "limits"
start wide "$B/oldparent" 65 -- -i 200
OLD=$(old_of wide)
wait_for bash -c "test \$(pgrep -P $OLD | wc -l) -eq 65"
out=$("$F" cutover --from "$OLD" wide 2>&1); rc=$?
check "--from refuses a parent with more than 64 children" test $rc -eq 1
check "the refusal names the limit" grep -q "$OLD has more than 64 children" <<<"$out"
check "nothing moved after the refusal" test "$(nkids "$OLD")" -eq 65
mapfile -t K < <(kids_of "$OLD")
expect_rc "65 named targets are a usage error" 2 "$F" cutover wide "${K[@]}"
expect_rc "64 named targets work" 0 "$F" cutover wide "${K[@]:0:64}"
check "all 64 were adopted" test "$(for k in "${K[@]:0:64}"; do ppid_of "$k"; done | grep -cx "$(sup_of wide)")" -eq 64
stop_family wide

start fds sh -c "$B/oldparent 64 -- -i 200 & $B/oldparent 64 -- -i 200 & $B/oldparent 1 -- -i 200 & wait"
SH=$(old_of fds)
wait_for bash -c "test \$(for p in \$(pgrep -P $SH); do pgrep -P \$p; done | wc -l) -eq 129"
A='' Bp='' C=
for p in $(kids_of "$SH"); do
    if [ "$(nkids "$p")" -eq 1 ]; then C=$p; elif [ -z "$A" ]; then A=$p; else Bp=$p; fi
done
AT=$(kids_of "$A")
expect_rc "a cutover taking 64 fds works" 0 "$F" cutover --from "$A" fds
expect_rc "a second one reaching 128 held fds works" 0 "$F" cutover --from "$Bp" fds
CT=$(kids_of "$C")
out=$("$F" cutover --dry-run --from "$C" fds 2>&1); rc=$?
check "dry run reports the held-fd limit" test $rc -eq 1 -a -n "$(grep 'more than 128' <<<"$out")"
out=$("$F" cutover --from "$C" fds 2>&1); rc=$?
check "a cutover past 128 held fds is refused" test $rc -eq 1
check "the refused cutover left everything in place" test "$(ppid_of "$CT")" = "$C"
# shellcheck disable=SC2086 # one pid per word
kill $AT
check "freed fds are released" wait_for bash -c "test \$('$F' inspect fds | grep -c ' pipe-r ') -le 64"
expect_rc "freed slots are reused by the next cutover" 0 "$F" cutover --from "$C" fds
stop_family fds

"$F" run -d -n evict -- sh -c "for i in \$(seq 300); do (sleep 0 &); done; touch '$FOCKER_RUNTIME_DIR/evict.done'; exec sleep 30" >/dev/null
wait_for test -e "$FOCKER_RUNTIME_DIR/evict.done"; sleep 0.3
members=$("$F" inspect evict | sed -n '/^members:/,/^$/p' | grep -Ec '^  [0-9]')
check "member history stops at 256" test "$members" -eq 256
check "the running first command is never evicted" grep -Eq '^  [0-9]+ +run ' <<<"$("$F" inspect evict)"
stop_family evict

echo "json"
# jsonok EXPR: stdin parses as JSON and the python expression EXPR (over d) is true
jsonok() { python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if ($1) else 1)" 2>/dev/null; }
if command -v python3 >/dev/null; then
    start jsonfam "$B/oldparent" 1 -- -i 50 $'a"b\\c\xff'
    T=$(ticker_of jsonfam)
    "$F" cutover jsonfam "$T" >/dev/null
    j=$("$F" inspect --json jsonfam)
    check "inspect --json is valid JSON, odd cmdline bytes included" jsonok "True" <<<"$j"
    check "inspect --json lists the adopted member" \
        jsonok "any(m['pid'] == $T and m['origin'] == 'adopted' and m['alive'] is True and m['exit_code'] is None for m in d['members'])" <<<"$j"
    check "inspect --json keeps the odd argument" jsonok "any('a\"b\\\\c�' in m['cmd'] for m in d['members'])" <<<"$j"
    check "inspect --json lists the held fd" \
        jsonok "d['held_fds'][0]['kind'] == 'pipe-r' and d['held_fds'][0]['child'] == $T and d['held_fds'][0]['child_fd'] == 1 and d['held_fds'][0]['log'].endswith('adopted-$T-fd1.log')" <<<"$j"
    check "inspect --json has every top-level key" \
        jsonok "set(d) == {'name', 'supervisor', 'state_dir', 'linger', 'members', 'held_fds'} and d['name'] == 'jsonfam'" <<<"$j"
    expect_rc "inspect --json on a missing family exits 1" 1 "$F" inspect --json nobody
    j=$("$F" ps --json)
    check "ps --json is valid JSON" jsonok "isinstance(d['families'], list)" <<<"$j"
    check "ps --json shows the running family" \
        jsonok "any(f['name'] == 'jsonfam' and f['running'] is True and f['children'] == 1 and f['pid'] == $(sup_of jsonfam) for f in d['families'])" <<<"$j"
    stop_family jsonfam
    check "ps --json without -a omits stopped families" \
        jsonok "not any(f['name'] == 'jsonfam' for f in d['families'])" <<<"$("$F" ps --json)"
    check "ps -a --json includes stopped families" \
        jsonok "any(f['name'] == 'jsonfam' and f['running'] is False and f['children'] is None and isinstance(f['status'], str) and f['status'] for f in d['families'])" <<<"$("$F" ps -a --json)"
else
    printf '  skip  json (needs python3)\n'
fi

echo "release"
held_fd() { "$F" inspect "$1" | awk -v k="$2" '$2 == k {print $1; exit}'; }   # NAME KIND -> focker's fd
start --linger rel "$B/oldparent" -I 1 -- -i 50 -e
SUP=$(sup_of rel) T=$(ticker_of rel)
out=$("$F" cutover rel "$T")
check "the cutover takes over stdin and stdout" bash -c "grep -q 'took over pipe-w' <<<'$out' && grep -q 'took over pipe-r' <<<'$out'"
FDW=$(held_fd rel pipe-w) FDR=$(held_fd rel pipe-r)
out=$("$F" release rel "$FDW"); rc=$?
check "release closes a held stdin" test $rc -eq 0
check "release says what it closed" grep -q "release: closed pipe-w fd $FDW (child $T fd 0)" <<<"$out"
check "the child sees EOF on stdin" wait_for bash -c "'$F' inspect rel | grep -Eq '^  $T .*exited 5'"
check "inspect stops listing a released fd" lacks " pipe-w " "$("$F" inspect rel)"
wait_for bash -c "! '$F' inspect rel | grep -q ' pipe-r '"
expect_rc "releasing an fd that EOF already closed exits 1" 1 "$F" release rel "$FDR"
expect_rc "releasing an unknown fd exits 1" 1 "$F" release rel 9999
check "the unknown fd is named" grep -q "focker holds no fd 9999" <<<"$("$F" release rel 9999 2>&1)"
expect_rc "release needs an fd" 2 "$F" release rel
expect_rc "release needs a numeric fd" 2 "$F" release rel x
stop_family rel

start --linger rel2 "$B/oldparent" -I 1 -- -i 50 -e
SUP=$(sup_of rel2) T=$(ticker_of rel2)
"$F" cutover rel2 "$T" >/dev/null
FDR=$(held_fd rel2 pipe-r)
expect_rc "release with one unknown fd still exits 1" 1 "$F" release rel2 "$FDR" 9999
check "the known fd was still released" lacks " pipe-r " "$("$F" inspect rel2)"
check "releasing a drained fd closes its log" bash -c "! ls -l /proc/$SUP/fd | grep -q 'adopted-$T-fd1.log'"
stop_family rel2

echo "cleanup after a crash"
start crash "$B/oldparent" 1 -- -i 50
COLD=$(old_of crash) CT=$(ticker_of crash)
kill -KILL "$(sup_of crash)"
check "a crashed supervisor's family is orphaned, not dead" wait_for bash -c "kill -0 $COLD 2>/dev/null && test \$(ps -o ppid= -p $COLD) -ne $(sup_of crash)"
kill_family "$FOCKER_RUNTIME_DIR/crash/"
check "cleanup kills the orphaned family by session" wait_for bash -c "! kill -0 $COLD 2>/dev/null && ! kill -0 $CT 2>/dev/null"
rm -rf "$FOCKER_RUNTIME_DIR/crash"

echo "signals to focker"
"$F" run -d -n fwd -- sleep 30 >/dev/null
kill -INT "$(sup_of fwd)"
check "kill -INT to focker is forwarded to the family" wait_for grep -qs "killed by SIGINT" "$FOCKER_RUNTIME_DIR/fwd/exit"
if command -v python3 >/dev/null; then
    python3 - "$F" "$FOCKER_RUNTIME_DIR/fgc/control.sock" <<'PY'
import os, pty, select, sys, time
pid, fd = pty.fork()
if pid == 0:
    os.execv(sys.argv[1], [sys.argv[1], "run", "-n", "fgc", "--", "sleep", "30"])
for _ in range(100):
    if os.path.exists(sys.argv[2]):
        break
    time.sleep(0.05)
os.write(fd, b"\x03")
while True:
    r, _, _ = select.select([fd], [], [], 5)
    try:
        if not r or not os.read(fd, 1024):
            break
    except OSError:
        break
sys.exit(os.waitstatus_to_exitcode(os.waitpid(pid, 0)[1]))
PY
    rc=$?
    check "Ctrl-C ends a foreground family with 130" test $rc -eq 130
    check "the exit record says SIGINT" grep -q "killed by SIGINT" "$FOCKER_RUNTIME_DIR/fgc/exit"
else
    printf '  skip  Ctrl-C (needs python3 for a pty)\n'
fi

echo "pty families"
expect_rc "run -t without -d is a usage error"   2 "$F" run -t -n x true
"$F" run -d -t -n tty1 -- sh -c 'tty; stty size; sleep 30' >/dev/null
check "a tty family's command sees a terminal" wait_for grep -qs '^/dev/pts/' "$FOCKER_RUNTIME_DIR/tty1/focker.log"
check "inspect lists the pty" grep -q 'pty' <<<"$("$F" inspect tty1 2>&1)"
T=$(old_of tty1)
check "the command is the terminal's foreground group" \
    test -n "$T" -a "$(ps -o tpgid= -p "$T" | tr -d ' ')" = "$(ps -o pgid= -p "$T" | tr -d ' ')"
stop_family tty1
"$F" run -d -t -n tty2 -- sh -c 'exit 4' >/dev/null
check "a tty family exits when its command does" wait_for grep -qs 'exited 4' "$FOCKER_RUNTIME_DIR/tty2/exit"
"$F" run -d -t -n tty3 -- sh -c 'seq 1 20000; echo last-words' >/dev/null
wait_for test -f "$FOCKER_RUNTIME_DIR/tty3/exit"
check "a tty family's last output reaches the log" grep -q '^last-words' "$FOCKER_RUNTIME_DIR/tty3/focker.log"
"$F" run -d -t -l -n tty4 -- sh -c 'exit 4' >/dev/null
wait_for bash -c "'$F' inspect tty4 2>/dev/null | grep -q 'exited 4'"
sleep 0.3
check "a lingering tty family outlives its command" test -S "$FOCKER_RUNTIME_DIR/tty4/control.sock"
stop_family tty4
if command -v python3 >/dev/null; then
    PTYRUN=(python3 "$ROOT/tests/ptyrun.py")
    out=$("${PTYRUN[@]}" -- sh -c 'tty; exit 3' 2>&1); rc=$?
    check "ptyrun runs a command on a terminal" test $rc -eq 3 -a -n "$(grep '^/dev/pts/' <<<"$out")"
else
    printf '  skip  ptyrun (needs python3)\n'
fi

echo "fg"
if command -v python3 >/dev/null; then
    PTYRUN=(python3 "$ROOT/tests/ptyrun.py")
    # reads lines until EOF and echoes them; a Ctrl-C is trapped, not fatal
    # shellcheck disable=SC2016 # expanded by the job's shell
    JOB='trap "echo got-INT; i=1" INT; echo ready; while i=; do if read l; then echo "echo:$l"; elif [ -z "$i" ]; then break; fi; done; exit 7'
    tty_fam() { # [-l] NAME CMD: start a tty family and wait for its first output
        local opt=(); [ "$1" = -l ] && { opt=(-l); shift; }
        "$F" run -d -t "${opt[@]}" -n "$1" -- sh -c "$2" >/dev/null && wait_for grep -qs . "$FOCKER_RUNTIME_DIR/$1/focker.log"
    }
    log_size() { stat -c %s "$FOCKER_RUNTIME_DIR/$1/focker.log"; }
    log_grew() { [ "$(log_size "$1")" -gt "$2" ]; }   # NAME SIZE

    tty_fam fg1 "$JOB"
    wait_for grep -qs '^ready' "$FOCKER_RUNTIME_DIR/fg1/focker.log"
    out=$("${PTYRUN[@]}" --stty-check --timeout 5 --send 'hi\n@0.3' --send '\x10a\n@0.5' --send '\x03@0.8' \
          --send '\x10\x11@1.2' -- "$F" fg fg1 2>&1); rc=$?
    check "fg shows the job's output and passes input" grep -q 'echo:hi' <<<"$out"
    check "Ctrl-C reaches the job" grep -q 'got-INT' <<<"$out"
    check "Ctrl-P then another key is passed through" grep -aq $'echo:\x10a' <<<"$out"
    check "Ctrl-P Ctrl-Q detaches, exit 0" test $rc -eq 0
    check "fg restores the terminal on detach" test $rc -ne 99
    check "the job outlives the detach" alive "$(old_of fg1)"

    tty_fam fgw 'while sleep 0.05; do stty size; done'
    out=$("${PTYRUN[@]}" --rows 33 --cols 77 --timeout 5 --send '\x10\x11@1' -- "$F" fg fgw 2>&1)
    check "the job sees fg's window size" grep -q '^33 77' <<<"$out"
    s1=$(log_size fgw)
    check "output goes back to the log after detach" wait_for log_grew fgw "$s1"
    s1=$(log_size fgw)
    "${PTYRUN[@]}" --timeout 5 --send '\x10\x11@1' -- "$F" fg fgw >/dev/null 2>&1
    check "the log pauses while a terminal is attached" test "$(log_size fgw)" -le $((s1 + 32))
    stop_family fgw

    tty_fam fg2 "$JOB"
    "${PTYRUN[@]}" --stty-check --timeout 6 -- "$F" fg fg2 >"$FOCKER_RUNTIME_DIR/fg2.out1" 2>&1 & p1=$!
    sleep 0.5
    out=$("${PTYRUN[@]}" --stty-check --timeout 5 --send 'x\n@0.5' --send '\x10\x11@1' -- "$F" fg fg2 2>&1); rc=$?
    wait "$p1"; rc1=$?
    check "a second fg takes over" test $rc1 -eq 0 -a $rc -eq 0
    check "the first fg says so" grep -q 'taken over by another terminal' "$FOCKER_RUNTIME_DIR/fg2.out1"
    check "the second fg gets the job's output" grep -q 'echo:x' <<<"$out"
    out=$("${PTYRUN[@]}" --stty-check --timeout 5 --send '\x04@0.3' -- "$F" fg fg2 2>&1); rc=$?
    check "fg exits with the job's exit code" test $rc -eq 7
    check "fg says how the job ended" grep -q 'fg2: exited 7' <<<"$out"

    tty_fam -l fg3 'echo bye; exit 7'
    wait_for bash -c "'$F' inspect fg3 | grep -q 'exited 7'"
    "${PTYRUN[@]}" --timeout 3 -- "$F" fg fg3 >/dev/null 2>&1; rc=$?
    check "fg after the job exited reports its status" test $rc -eq 7
    stop_family fg3

    "$F" run -d -n fg4 -- sleep 30 >/dev/null
    out=$("${PTYRUN[@]}" --timeout 3 -- "$F" fg fg4 2>&1); rc=$?
    check "fg on a family without a terminal exits 1" test $rc -eq 1
    check "and says it has no terminal" grep -q 'fg4 has no terminal' <<<"$out"
    stop_family fg4
    expect_rc "fg needs a terminal"                1 "$F" fg fg1 </dev/null
    expect_rc "fg on an unknown family"            1 "${PTYRUN[@]}" --timeout 3 -- "$F" fg nosuchfam
    expect_rc "fg without a name"                  2 "$F" fg

    "${PTYRUN[@]}" --stty-check --timeout 6 -- "$F" fg fg1 >/dev/null 2>&1 & p1=$!
    wait_for pgrep -P "$p1" >/dev/null; sleep 0.3
    kill -TERM "$(pgrep -P "$p1")"
    wait "$p1"; rc=$?
    check "fg restores the terminal when killed" test $rc -ne 99 -a $rc -ne 124
    "${PTYRUN[@]}" --stty-check --timeout 6 -- "$F" fg fg1 >"$FOCKER_RUNTIME_DIR/fg1.out" 2>&1 & p1=$!
    sleep 0.5
    kill -KILL "$(sup_of fg1)"
    wait "$p1"; rc=$?
    check "fg restores the terminal when the supervisor dies" test $rc -eq 1
    check "fg says the supervisor went away" grep -q 'supervisor went away' "$FOCKER_RUNTIME_DIR/fg1.out"

    "$F" run -d -t -n fgcut -- "$B/oldparent" 2 -- -i 50 >/dev/null
    wait_for has_grandchild "$(sup_of fgcut)"
    SUP=$(sup_of fgcut) P=$(old_of fgcut); T=$(kids_of "$P" | head -1)
    "$F" cutover --wait -t 5 fgcut "$T" >/dev/null 2>&1 & cpid=$!
    wait_for frozen_by "$T" "$SUP"
    out=$("${PTYRUN[@]}" --timeout 4 --send '\x10\x11@1' -- "$F" fg fgcut 2>&1); rc=$?
    check "fg is answered during a cutover" test $rc -eq 0 -a -n "$(grep 'tick' <<<"$out")"
    kill "$P"; wait "$cpid"
    stop_family fgcut
else
    printf '  skip  fg (needs python3)\n'
fi

echo
echo "$pass passed, $fail failed"
for t in "${failed[@]}"; do echo "  failed: $t"; done
[ "$fail" -eq 0 ]
