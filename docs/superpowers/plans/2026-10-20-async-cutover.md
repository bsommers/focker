# Phase 7 Async Cutover Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** While a cutover waits for the old parent to exit, the supervisor keeps draining held fds and answering read-only requests, and refuses a second cutover with `busy`.

**Architecture:** `cutover()` in `src/cutover.c` becomes a job with `cutover_start` / `cutover_step` / `cutover_cancel`. Only the wait (step 3) spans loop rounds; validation, freeze, fd takeover and thaw stay inline because they are bounded. `supervisor.c` keeps one in-flight job with its client fd and transcript, steps it every 20 ms round, and replies when it ends.

**Tech Stack:** C11, the existing poll loop in `supervisor_run`, the `tests/run.sh` integration harness.

**Spec:** `.planning/REQUIREMENTS.md` (ASYNC-01), `.planning/ROADMAP.md` Phase 7, `.planning/phases/07-async-cutover/07-CONTEXT.md` (D-01..D-06)

## Global Constraints

- ASYNC-01: "A cutover runs as a non-blocking state machine inside the poll loop. During a cutover (up to `--timeout`), held fds keep draining and other read-only requests (`info`, `inspect`, `members`) are answered. A second cutover is refused with `busy`"
- Roadmap: "Every v1.0 cutover check still passes, under `make ci`"
- Transcript lines, exit codes (0 / 1 runtime / 2 usage) and the control protocol's reply format don't change. The refusal text is `busy (a cutover is in progress)` (D-03).
- No new dependencies. No new privileges. `make ci` must pass.

## Review Focus

- **A frozen target is SIGKILLed during the wait:** it must not show up as a family member, and the cutover exits 1. (Task 3, `a target killed during the wait is not a member`.)
- **SIGTERM to the supervisor during the wait:** the cutover rolls back first (target thawed, not left stopped), then the signal is forwarded. (Task 3, `SIGTERM during the wait rolls back first`.)
- **The client is killed mid-wait:** the cutover still completes and the supervisor survives. (Task 2, `a vanished client doesn't stop the cutover`.)
- **Mutating requests during the wait** (`release`, `stop`, `handover`, `--dry-run`): `busy`, exit 1, nothing changed. (Task 2.)
- **A drained fd hits EOF during the wait:** the slot closes as usual, and rollback must not close a slot that is already `-1`. (Task 1: rollback guards `fd >= 0`; covered by the existing suite plus Task 2's live drain.)

---

### Task 1: The cutover as a job (no behavior change)

**Files:**
- Modify: `src/cutover.c` (`cutover` → job functions), `src/focker.h` (prototypes), `src/supervisor.c` (`do_cutover` drives the job to completion)
- Test: the existing suite (`make ci`). This is a refactor; no new test.

**Interfaces:**
- Produces, in `src/focker.h`:
  - `#define CUTOVER_PENDING (-2)`
  - `struct cutover_job;` (opaque; defined in `cutover.c`, holds what `cutover()` kept in locals: targets, old parents, bystanders, target and parent pidfds, the `frozen` array, `stolen_base`, `t0`, a copy of the opts, the family pointer)
  - `int cutover_start(struct family *f, const struct cutover_opts *o, const pid_t *pids, int npids, struct sbuf *out, struct cutover_job **job);` Runs steps 0-3 (through signalling the old parents). Returns 0 or 1 when the cutover already ended (validation failure, dry run, nothing to do, rollback before the wait), with `*job = NULL`. Otherwise returns `CUTOVER_PENDING` with `*job` set.
  - `int cutover_step(struct cutover_job *job, struct sbuf *out);` One non-blocking check: if every target has left its old parent, or `timeout_ms` has passed since `t0`, it runs verify/thaw/bystanders (or the timeout rollback), frees the job, and returns 0 or 1. Otherwise returns `CUTOVER_PENDING`.
  - `int cutover_cancel(struct cutover_job *job, const char *why, struct sbuf *out);` Appends `cutover: cancelled by WHY` and does the rollback (same lines as today), frees the job, returns 1.
  - `bool cutover_owns(const struct cutover_job *job, pid_t pid);` True if `pid` is one of the job's targets.
- `cutover()` is removed.

- [ ] **Step 1:** Move the locals into `struct cutover_job` (heap, `calloc`), and split the body at the wait loop. The internal `poll(pfd, nparents, 20)` goes away: the caller paces the steps. Rollback closes `stolen[i].fd` / `logfd` only when `>= 0`.
- [ ] **Step 2:** `do_cutover` in `supervisor.c`: call `cutover_start`; while it returns `CUTOVER_PENDING`, `sleep_ms(20)` and call `cutover_step`. Keep the `logmsg("cutover %s", ...)` on the final rc.
- [ ] **Step 3:** `make ci`. Expected: exit 0, `84 passed, 0 failed` and `157 passed, 0 failed` under all three builds, `no sanitizer reports`.
- [ ] **Step 4:** Commit: `cutover: run as a job (start/step/cancel); no behavior change`

### Task 2: Step the job from the poll loop

**Files:**
- Modify: `src/supervisor.c` (in-flight job, busy refusals, loop timeout and exit guard, reply on completion), `docs/USAGE.md` (protocol: "Requests are served one at a time" → a cutover in flight answers read-only requests and refuses the rest with `busy`), `docs/ARCHITECTURE.md` (§4.3: the wait runs in the loop; §2 diagram line for requests), `.planning/codebase/CONCERNS.md` (strike "Synchronous request handling", like the Phase 5 entry), `CHANGELOG.md` (Unreleased, Changed)
- Test: `tests/run.sh` (new section `async cutover`)

**Interfaces:**
- Consumes: `cutover_start`, `cutover_step`, `CUTOVER_PENDING` (Task 1).
- Produces: in `supervisor.c`, `static struct { struct cutover_job *job; int cfd; struct sbuf out; } inflight;` (`job == NULL` means idle), and `static void finish_inflight(int rc);`, which writes `ok`/`err` plus the transcript to `cfd`, closes it, frees the buffer, and logs `cutover succeeded|failed`. Task 3 uses both.

- [ ] **Step 1: Write the failing tests** (`tests/run.sh`, section `async cutover`, placed after `cutover options`). Helper `async_fam NAME` starts `NAME` as `sh -c "$B/oldparent 1 -- -i 50 & $B/oldparent 1 -- -i 50 & wait"`, sets `SUP`, `P1`/`P2` (the two oldparents, `kids_of` the sh, sorted) and `T1`/`T2` (each one's ticker), and cuts over `--from $P1` so T1's pipe is held and draining. Helper `frozen_by PID SUP`: `/proc/PID/status` has `TracerPid:\t$SUP`. Checks on family `async`:
  - Start `"$F" cutover --wait -t 5 async "$T2" >"$TMP/async.out" 2>&1 &` (remember `$!`), then `wait_for frozen_by "$T2" "$SUP"`.
  - `drains keep running during a cutover`: the size of `adopted-$T1-fd1.log` grows across `sleep 0.5`.
  - `inspect answers during a cutover`: `timeout 2 "$F" inspect async` exits 0.
  - `info answers during a cutover`: `timeout 2 "$F" ps` lists `async`.
  - `a second cutover is busy`: `"$F" cutover async "$T1"` exits 1 and prints `busy (a cutover is in progress)`.
  - `a dry run is busy too`, `release is busy`, `stop is busy`: each exits 1 with `busy`; `stop` leaves the family running (`test -S .../async/control.sock`).
  - `kill "$P2"`; `the waiting cutover completes`: `wait $!` gives 0, and `$TMP/async.out` has `adopted $T2`.
  - On a fresh family `async2`: start the same background cutover, `wait_for frozen_by`, `kill -9` the background client, `kill "$P2"`. `a vanished client doesn't stop the cutover`: `wait_for` `ppid_of "$T2"` = `$SUP`, and `"$F" inspect async2` exits 0.
- [ ] **Step 2:** `make test`. Expected: the drain, inspect, info and busy checks fail (the supervisor is blocked in `do_cutover` for 5 s, so `timeout 2` expires and the second cutover runs after the first one instead of being refused). `a vanished client doesn't stop the cutover` may already pass: it is a guard for the new code path.
- [ ] **Step 3: Implement.** `handle_client` for `cutover`: if `inflight.job` is set, reply `busy (a cutover is in progress)` with rc 1. Otherwise call `cutover_start`; on `CUTOVER_PENDING`, store the job, `cfd` and transcript in `inflight` and return without replying or closing. `release`, `stop`, `handover`: `busy` while a job is in flight. The loop: poll timeout 20 ms while `inflight.job` is set; after the poll and the request/drain handling, `cutover_step` and `finish_inflight` when it returns a final rc. The loop's exit check also requires `inflight.job == NULL`.
- [ ] **Step 4:** `make ci`. Expected: exit 0, every check passes under all three builds.
- [ ] **Step 5:** Commit: `supervisor: step the cutover from the poll loop; busy while one is in flight (ASYNC-01)`

### Task 3: Reaper and signals during an in-flight cutover

**Files:**
- Modify: `src/supervisor.c` (`reap_children` skips owned pids; terminating signals cancel the job before forwarding), `src/cutover.c` (when a job ends, consume the tracer exit notification of any target that died while frozen), `docs/ARCHITECTURE.md` (§5.1 reaping and §5.4 signals), `CHANGELOG.md`
- Test: `tests/run.sh` (section `async cutover`)

**Interfaces:**
- Consumes: `inflight`, `finish_inflight` (Task 2); `cutover_owns`, `cutover_cancel` (Task 1).

- [ ] **Step 1: Write the failing tests:**
  - `a target killed during the wait is not a member`: `async_fam async3`, background `cutover --wait -t 1 async3 "$T2"`, `wait_for frozen_by`, `kill -9 "$T2"`. `wait $!` gives 1. `"$F" inspect async3` has no row starting with `$T2` (`! grep -Eq "^ +$T2 "`).
  - `SIGTERM during the wait rolls back first`: `async_fam async4`, background `cutover --wait -t 5 async4 "$T2"`, `wait_for frozen_by`, `kill -TERM "$SUP"`. `wait $!` gives 1 within 3 s, and the transcript has `cancelled by SIGTERM` and `rolled back`. `T2 is not left stopped`: either it's gone, or `ps -o stat=` doesn't start with `T` or `t`, and its `TracerPid` is 0.
  - Clean up both with `kill_family`.
- [ ] **Step 2:** `make test`. Expected: the SIGTERM check fails (the signal is forwarded while the target is still frozen, and the cutover waits out its timeout). The killed-target check fails if the reaper registers the tracee's exit as an orphan member.
- [ ] **Step 3: Implement.** `reap_children`: when a job is in flight and `cutover_owns(job, pid)`, stop peeking for this round (`return`), leaving the event for the job. In the signalfd handler, for TERM/INT/HUP/QUIT with a job in flight: `finish_inflight(cutover_cancel(job, signame(sig), &inflight.out))` before forwarding. In `cutover.c`, every `cutover_step` (and the end of a job: success, failure, timeout or cancel) does `waitpid(pid, NULL, __WALL | WNOHANG)` for each target that is no longer alive (`pinned_alive` false), consuming the tracer's notification. `waitid(P_ALL)` keeps returning the same pending event, so the reaper's `return` would otherwise stall all reaping until the job ends; this way it lasts at most one 20 ms round.
- [ ] **Step 4:** `make ci`. Expected: exit 0.
- [ ] **Step 5:** Commit: `cutover: a frozen target's exit and supervisor signals are handled mid-wait; Phase 7 complete`, with the `.planning` updates (REQUIREMENTS ASYNC-01, ROADMAP, STATE, `07-01-SUMMARY.md`).
