# Phase 5: Fd control and JSON - Context

**Gathered:** 2026-10-20
**Status:** Ready for planning

<domain>
## Phase Boundary

FD-02 `focker release NAME FD...` and OUT-01 `ps --json` / `inspect --json`. No change to the existing text
output or transcript lines.

</domain>

<decisions>
## Implementation Decisions

### release (FD-02)
- **D-01:** An fd is named by focker's own fd number, the `FD` column of `inspect` and `FOCKER_FDS`, because
  that is unique, whereas a child can have the same fd number taken over from two parents. Several can be given at once.
- **D-02:** New control request `release FD...`. The supervisor closes the fd and its drain log, marks the slot
  free (fd = logfd = -1, so Phase 2's compaction reuses it), and logs it. Each line of the reply is
  `release: closed KIND fd N (child PID fd M)`. An fd it doesn't hold gives `release: focker holds no fd N`
  and exit 1, but the other fds are still released.
- **D-03:** Fixtures: `oldparent -I` also gives each child a stdin pipe whose write end the parent keeps.
  `ticker -e` reads stdin in a thread and exits with status 5 on EOF.

### JSON (OUT-01)
- **D-04:** `inspect --json` is built by the supervisor (request `inspect json=1`), because it owns the
  member table. `ps --json` is built by the client from `info` replies and the state-dir files, as `ps` is now.
- **D-05:** Schema, with keys always present (`null` when unknown):
  - inspect: `{"name","supervisor","state_dir","linger","members":[{"pid","origin":"run|adopted|orphan","from","alive","status","exit_code","signal","cmd"}],"held_fds":[{"fd","kind","child","child_fd","from","log"}]}`
  - ps: `{"families":[{"name","pid","running","status","children","cmd"}]}`. Without `-a`, only running families.
- **D-06:** Strings go through one escaper, `sb_json_str()` in util.c: `"` and `\` are escaped, control
  characters become `\u00XX`, and invalid UTF-8 (cmdlines are arbitrary bytes) becomes `�`, so the
  output is always valid JSON.
- **D-07:** Tests validate with `python3 -m json.tool` and are `skip`ped without python3, like the Ctrl-C test.

### Claude's Discretion
- Field order, help text wording.

</decisions>
