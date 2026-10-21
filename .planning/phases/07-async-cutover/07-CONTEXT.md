# Phase 7: Async cutover - Context

**Gathered:** 2026-10-20
**Status:** Ready for planning

<domain>
## Phase Boundary

ASYNC-01: the wait for the old parents to exit (step 3 of the cutover, up to `--timeout`) runs inside the
supervisor's poll loop instead of blocking it. During it, held fds keep draining and `info`, `inspect` and
`members` are answered. A second cutover is refused with `busy`. Validation, the freeze, the fd takeover
and the thaw stay inline: they are bounded (milliseconds with ptrace; at most 2 s for `--freeze signal`
and 250 ms for the swallow loop).

</domain>

<decisions>
## Implementation Decisions

- **D-01:** `cutover()` is split into a job: `cutover_start` (validate, plan, freeze, take fds, signal the
  old parents), `cutover_step` (one non-blocking check of the wait; on adoption or timeout it verifies,
  thaws and reports), and `cutover_cancel` (roll back now). The job owns everything `cutover()` kept in
  locals. Task 1 is a pure refactor: the supervisor still blocks, stepping the job every 20 ms.
- **D-02:** The supervisor keeps at most one job, with the client's fd and the transcript buffer. While a
  job is in flight the poll timeout is 20 ms (instead of 1000) and the job is stepped every round. The
  reply is written when the job ends. No pidfds are added to the poll set.
- **D-03:** While a job is in flight, `cutover`, `release`, `stop` and `handover` are refused with
  `busy (a cutover is in progress)` and exit 1. `cutover --dry-run` is refused too: it shares the planning
  buffers. `info`, `inspect` and `members` are answered.
- **D-04:** The reaper skips the job's targets (they are focker's tracees, so their exits reach focker's
  `waitid(P_ALL)`). A target that dies while frozen is never recorded as a family member: the job consumes
  the tracer's exit notification when it ends.
- **D-05:** A terminating signal to the supervisor (TERM, INT, HUP, QUIT) while a job is in flight cancels
  it: the transcript says `cutover: cancelled by SIGTERM` and then the usual rollback line, the client gets
  exit 1, and only then is the signal forwarded and lingering ended, as today.
- **D-06:** The supervisor never leaves its loop while a job is in flight. A client that disconnects
  mid-cutover doesn't stop the cutover. Its reply is dropped (SIGPIPE is already ignored).

</decisions>
