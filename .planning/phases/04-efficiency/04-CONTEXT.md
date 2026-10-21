# Phase 4: Efficiency - Context

**Gathered:** 2026-10-20
**Status:** Ready for planning

<domain>
## Phase Boundary

PERF-01 (child enumeration through `/proc/PID/task/*/children`, with the full scan as fallback, results
identical and tested both ways) and PERF-02 (measure the idle supervisor's CPU and wakeups and document
them; document the limits in USAGE and LIMITATIONS).

</domain>

<decisions>
## Implementation Decisions

- **D-01:** The fast path is used everywhere `proc_children` is. The kernel documents the `children` list as
  reliable only while the children are stopped or frozen, but the PPid scan is just as racy (a fork during
  the scan can be missed), so this adds no new race.
- **D-02:** Fallback only when no `children` file exists (kernel without `CONFIG_PROC_CHILDREN`), not on
  an empty list.
- **D-03:** Both modes return the lowest `max` PIDs, sorted and deduped (the scan used to return an
  arbitrary subset when capped).
- **D-04:** `proc_children_scan_only` (a global, false by default) exists for tests only. No user-facing knob.
- **D-05:** Measurement uses `/proc/PID/schedstat` (ns on CPU) and the context-switch counters, not
  `utime`, which at 100 Hz rounds an idle supervisor down to 0.
- **D-06:** The detailed implementation plan is the superpowers plan
  `docs/superpowers/plans/2026-10-20-efficiency.md` (user: "add superpowers plan to this").

</decisions>
