# Phase 2: Test coverage - Context

**Gathered:** 2026-10-20
**Status:** Ready for planning

<domain>
## Phase Boundary

Cover every gap listed in `.planning/codebase/TESTING.md` (TST-01..06): `--no-drain`, `--no-suppress`,
`--wait`, `logs -f`, handover exec failure, long runtime paths, the `MAX_*` limit edges, foreground
Ctrl-C, and parser unit tests. Bugs the new tests expose are fixed in this phase when the fix is local.

</domain>

<decisions>
## Implementation Decisions

### Bugs found while planning (fixed here, each with a test)
- **D-01:** `proc_children` stops at `max` silently, so `cutover --from` a parent with more than 64
  children froze only the first 64; the rest were orphaned unfrozen without fd takeover. The cutover now
  asks for `MAX_PIDS + 1` and refuses (`N has more than 64 children`).
- **D-02:** held-fd slots were never reclaimed (`nstolen` only grows), so after 128 takeovers in a
  family's lifetime no more fds could be taken. Closed slots are compacted at the start of a cutover.
- **D-03:** a cutover that would exceed `MAX_STOLEN` used to go ahead without the extra fds, leaving
  those children to die of SIGPIPE. It is now refused before anything is frozen (also in `--dry-run`).

### Test harness
- **D-04:** fix `start()`: `wait_for test "$(...)"` evaluated once. Waits now re-evaluate each try.
- **D-05:** foreground Ctrl-C needs a real tty; the test uses python3's `pty` module and is skipped
  (reported as `skip`) when python3 is missing.
- **D-06:** `oldparent` accepts up to 128 children (was 16) for the limit tests.

### Unit tests (TST-06)
- **D-07:** `procfs.c` reads from `proc_root` (default `/proc`), so `tests/unit.c` can point it at a
  fixture tree built in a temp dir. `unixdiag.c` is tested against a live `socketpair`.
- **D-08:** `make unit` builds and runs `build/unit`; `make test` runs unit then integration.

### Claude's Discretion
- Fixture layout, check names, how the limit tests group processes.

</decisions>

<canonical_refs>
## Canonical References

- `.planning/REQUIREMENTS.md` (TST-01..06), `.planning/codebase/TESTING.md` (Gaps)
- `.planning/codebase/CONCERNS.md` (the `wait_for` note)

</canonical_refs>
