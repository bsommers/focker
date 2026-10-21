---
phase: 03-sanitizers-and-ci
plan: 01
subsystem: build
tags: [asan, ubsan, fanalyzer, shellcheck, ci]
requires: [02-01, 02-02]
provides: [make asan, make lint, make ci, disabled GitHub workflow]
affects: [04-efficiency]
key-files:
  created: [tests/sanitize.sh, ci/github-actions.yml]
  modified: [Makefile, tests/run.sh, src/main.c, src/supervisor.c, .gitignore]
key-decisions:
  - "Separate ASan and UBSan builds (UBSan ignores log_path when combined with ASan)"
  - "Workflow kept out of .github/workflows/ at the user's request"
requirements-completed: [BLD-01, BLD-02, BLD-03]
completed: 2026-10-20
---

# Phase 3 Plan 01: Sanitizers and CI Summary

**`make asan`, `make lint` and `make ci` all pass locally. The GitHub workflow is written but disabled.**

## Accomplishments
- `make asan`: clean `build-asan/` (ASan+LSan) and `build-ubsan/` builds, each running 35 unit + 117 integration checks through `tests/sanitize.sh`. Zero reports.
- `make lint`: `-fanalyzer -Werror` on every C file plus shellcheck. Fixed a real fd leak (the `run -d` ready pipe when `fork` fails), restructured `setup_socket` around a local fd (an analyzer false positive on the global), and fixed three shellcheck findings.
- `FK_CFLAGS`: `make CFLAGS=...` no longer drops `-std=c11 -D_GNU_SOURCE`. `tests/run.sh` honours `FOCKER_BUILD`.
- `ci/github-actions.yml` runs lint, test and asan on push and pull request. It is disabled because it sits outside `.github/workflows/`, and its header says how to enable it.

## Deviations from Plan
- One combined `-fsanitize=address,undefined` build was planned. A planted heap overflow in a detached supervisor showed that UBSan ignores `log_path` in combined builds, so its reports landed in a `focker.log` the suite deletes, and the run passed. Split into two builds; the planted bug is now reported by both.
- Success criterion 3 (verify with `act` or a first push) was replaced by `make ci`, per the user. `act` isn't installed, and there is no remote.

## Verification
- A planted `volatile` heap overflow in `do_inspect` was caught by both builds (ASan: 7 report files; UBSan: report files with stack traces). Removed afterwards.
- `make ci` passes end to end in 63 s. The workflow YAML parses (PyYAML).
