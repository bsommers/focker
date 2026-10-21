# Phase 3: Sanitizers and CI - Context

**Gathered:** 2026-10-20
**Status:** Ready for planning

<domain>
## Phase Boundary

`make asan` (ASan+UBSan build plus the full suite, failing on any sanitizer report), `make lint`
(gcc `-fanalyzer` plus shellcheck), and a CI workflow that runs both. Everything runs locally.

</domain>

<decisions>
## Implementation Decisions

### User constraint
- **D-01:** "do phase 3 all locally, no CI on github yet. if you build github CI, leave it disabled."
  The workflow lives at `ci/github-actions.yml`, outside `.github/workflows/`, so GitHub never runs
  it. Enabling it means copying it into `.github/workflows/` (documented). `make ci` runs the same
  steps locally. Success criterion 3 ("verified with act or a first push") is replaced by "verified by
  running `make ci` locally"; `act` isn't installed and there is no remote.

### Builds
- **D-02:** sanitizer objects go to `build-asan/` (`BUILD` is overridable) so they never mix with `build/`.
- **D-03:** project flags move to `FK_CFLAGS`, so `make CFLAGS=...` no longer drops `-std=c11 -D_GNU_SOURCE`.
- **D-04:** sanitizer reports are written with `log_path` into a temp dir, because detached supervisors'
  stderr goes to a `focker.log` that the suite deletes. Any report file fails `make asan`.
- **D-05:** `tests/run.sh` takes the build dir from `FOCKER_BUILD` (default `build`).

### Lint
- **D-06:** `-fanalyzer -Werror` on `src/*.c`, `tests/unit.c`, `examples/*.c`; shellcheck on the shell scripts.
  Findings are fixed, not suppressed, unless they are false positives that can't be restructured away.

</decisions>
