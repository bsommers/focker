---
phase: 05-fd-control-and-json
plan: 01
subsystem: cli
tags: [json, fds]
requires: [04-01]
provides: [focker release, ps --json, inspect --json, sb_json_str]
affects: [06-loopback-tcp]
key-files:
  modified: [src/util.c, src/focker.h, src/supervisor.c, src/client.c, src/main.c, examples/ticker.c, examples/oldparent.c, tests/unit.c, tests/run.sh, docs/USAGE.md, README.md, docs/HOWTO.md, CHANGELOG.md]
key-decisions:
  - "release takes focker's own fd numbers (inspect FD column)"
  - "inspect --json built by the supervisor, ps --json by the client"
requirements-completed: [FD-02, OUT-01]
completed: 2026-10-20
---

# Phase 5 Plan 01: Fd control and JSON Summary

**`focker release`, `ps --json` and `inspect --json` are shipped, each tested first. Steps are in docs/superpowers/plans/2026-10-20-fd-control-and-json.md.**

## Accomplishments
- `sb_json_str`: escapes and validates UTF-8 (overlongs, surrogates, truncation become U+FFFD). 10 unit checks.
- `inspect --json` / `ps --json`, schema in USAGE. Tested with a cmdline holding `"`, `\` and `\xff`.
- `focker release NAME FD...`: EOF reaches the child's stdin (it exits 5), an fd already closed by EOF and an unknown fd both exit 1, a mixed list still releases the known fds, and a drained fd's log is closed.

## Deviations / found along the way
- A stale-index drain after serving a request (with blocking fds) is fixed: no draining in a round that served a request.

## Verification
- `make ci` exit 0: 53 unit + 144 integration checks under the plain, ASan and UBSan builds.
