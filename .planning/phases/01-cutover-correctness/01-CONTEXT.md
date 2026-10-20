# Phase 1: Cutover correctness - Context

**Gathered:** 2026-10-20
**Status:** Ready for planning

<domain>
## Phase Boundary

Fix the four known edge cases in `src/cutover.c` (REL-01..04) so a cutover never loses a child's exit
status, never cancels a job-control stop, never acts on a recycled PID, and never hides a signal it
could not swallow. Each fix ships with a regression test in `tests/run.sh`. No new CLI surface.

</domain>

<decisions>
## Implementation Decisions

### REL-01: exit status of a target that dies during thaw
- **D-01:** ptrace waits peek with `waitid(WNOWAIT)` first. An exit of a thread-group leader that is
  already focker's child is left unreaped, so `reap_children()` records it with the usual bookkeeping
  (member status, log line, held fds released). Exits focker sees only as the tracer are consumed as
  before, since the real parent can't reap until the tracer has.
- **D-02:** fixture: `ticker -x SIG` exits with status 7 when SIG arrives, which lets a passed-through
  signal kill the target inside the swallow loop deterministically.

### REL-02: job-control stops during the cutover
- **D-03:** SIGSTOP, SIGTSTP, SIGTTIN and SIGTTOU are never swallowed (they are a request, not fallout of
  the parent's death). In the swallow loop a group-stop event-stop is resumed with `PTRACE_LISTEN`,
  not `PTRACE_CONT`. Detach then leaves the process stopped, and the transcript says so.
- **D-04:** `--freeze signal` is unchanged (its SIGCONT on thaw is inherent to the method).

### REL-03: pin targets with a pidfd
- **D-05:** `pidfd_open` every target before any `/proc` read. After validation and after the freeze,
  `pidfd_send_signal(fd, 0)` must still succeed, otherwise the PID died (and may be reused); the cutover
  refuses or rolls back with `cutover: N exited before it could be frozen`.
- **D-06:** test hook `FOCKER_TEST_STALL_MS` (read by the supervisor) sleeps between validation and
  freeze so a test can kill the target inside that window. PID reuse itself can't be forced without
  `CAP_SYS_ADMIN`, so the test covers the detection path, not reuse.

### REL-04: signals that were already pending
- **D-07:** the kernel discards a second standard signal while one is pending, so a re-send can't be
  observed. focker reports every non-benign signal pending both before the freeze and at thaw:
  `cutover: SIGX was already pending for N before the freeze; a copy sent during the cutover can't be
  told apart from it and was not swallowed`.

### Claude's Discretion
- Exact transcript wording, helper names, fixture flags.

</decisions>

<specifics>
## Specific Ideas

Transcript lines keep the `cutover: ` prefix and stable shapes (scripts grep them).

</specifics>

<canonical_refs>
## Canonical References

- `.planning/REQUIREMENTS.md` (REL-01..04)
- `.planning/codebase/CONCERNS.md` (Known Bugs, Security Considerations)
- `docs/ARCHITECTURE.md` §4.2, §4.4
- ptrace(2): "Group-stop", PTRACE_LISTEN; waitid(2) WNOWAIT; pidfd_send_signal(2)

</canonical_refs>
