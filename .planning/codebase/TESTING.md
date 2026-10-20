---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# Testing Patterns

**Analysis Date:** 2026-10-20

## Test Framework

- `tests/run.sh`, a self-contained bash harness. `make test` builds everything first.
- Each run uses a private `FOCKER_RUNTIME_DIR=$(mktemp -d)`. An EXIT trap SIGKILLs every supervisor and its children and removes the dir.
- Runs in about 8 seconds without root. Output is `ok`/`FAIL` per check, then a summary. Exits nonzero on any failure.

## Helpers

| helper | purpose |
|---|---|
| `check NAME cmd...` | pass if the command succeeds |
| `expect_rc NAME CODE cmd...` | pass if the exit code matches (on failure, prints the first lines of output) |
| `wait_for cmd...` | retry for up to 3s, the only sanctioned way to wait for asynchronous process state |
| `start NAME cmd...` | `focker run -d`, then wait until children and grandchildren exist |
| `sup_of` / `old_of` / `ticker_of` / `kids_of` / `ppid_of` / `alive` | process lookups via the state dir, pgrep and ps |
| `stop_family NAME` | `focker stop`, then wait for the control socket to disappear |

## Fixtures

The `examples/` programs are the fixtures:
- `ticker` (pdeathsig with `-d`, extra threads with `-t`)
- `oldparent` (pipes, or socketpairs with `-s`)
- `newparent` (handover target)

Shell one-liners model specific parent behaviors: a parent that ignores TERM, one that kills its child on TERM, and nested parents.

## Coverage (71 checks)

- CLI contract: usage errors exit 2, runtime errors exit 1, every `--help` succeeds, bad names, signals, timeouts and `FOCKER_FREEZE`.
- run: foreground exit status passthrough, 127 for a missing program, duplicate names, rm.
- cutover: dry run changes nothing; pipe takeover and drain; bystanders; tree annotations; socketpair peer takeover; a 5-thread freeze; pdeathsig swallowed with ptrace and fatal with signal; shutdown-kill swallowed; timeout rollback; KILL escalation; outsider and self refused; the target/parent conflict; grandchild generation-skip; `--from`.
- handover: PID kept, program replaced, `FOCKER_FDS` delivered, the new parent relays and reaps, and `ps -a` records it.

## Gaps

- No unit tests for `procfs.c` / `unixdiag.c` parsers in isolation.
- Untested: `--no-drain`, `--no-suppress`, `--wait` success, `logs -f`, the handover exec-failure recovery path, the long-path `control_addr` fallback, the `MAX_*` limit edges, and foreground Ctrl-C behavior.
- No CI, and no sanitizer build (ASan/UBSan) target.

## Writing new tests

Add them in the matching `echo "section"` block of `tests/run.sh`. Always assert on kernel-observable state (`ppid_of`, `alive`, the `ps` state column), not only on focker's transcript. Use `wait_for` instead of bare `sleep` wherever possible.

---

*Testing analysis: 2026-10-20*
