# Checkpoint: focker, 2026-10-20 (after Phase 3)

## Goal (user's words)
"fully reparent a child process from an old parent to a new parent ... checkpoint suspend then cut over",
then "make a command line utility to help me. write up full documentation for it. initialize a git repo ...
think docker, but its meet the fockers", then "onboard this to opengsd", then "go" (Phase 1),
"start phase 2", "do phase 3 all locally, no CI on github yet. if you build github CI, leave it disabled".

## Done (verified)
- `focker` C CLI (`src/`), fixtures (`examples/`), docs (README + `docs/`), GSD `.planning/` (Hardening milestone).
- Phase 1 `6144a6c` (REL-01..04), Phase 2 `41b1de1` (TST-01..06), Phase 3 `ac2cc7f` (BLD-01..03), each with a
  phase dir in `.planning/phases/` (CONTEXT, PLAN, SUMMARY). gsd-tools `roadmap analyze --raw` reports 1-3 complete.
- Baseline: `make ci` (= lint + test + asan) exit 0 in about 63 s; `make test` = unit `35 passed, 0 failed` +
  integration `117 passed, 0 failed` (about 19 s).

## User decisions (verbatim)
- Milestone: **"Hardening (Recommended)"**. GSD install: **"Artifacts only (Recommended)"** (no npx install).
- "do phase 3 all locally, no CI on github yet. if you build github CI, leave it disabled." So the workflow is
  `ci/github-actions.yml`, outside `.github/workflows/`. Don't move it.

## Findings worth keeping (dead ends)
- PTRACE_LISTEN for group-stops in the swallow loop is WRONG: still-pending signals aren't swallowed, and SIGCONT
  kills the child. PTRACE_CONT stays (detach re-arms the stop). A test guards this.
- One combined `-fsanitize=address,undefined` build silently drops UBSan reports (they ignore `log_path`). Keep
  two builds. At -O1 a non-volatile planted overflow is optimized away, so plant bugs as `volatile`.
- `wait_for test "$(...)"` expands once. Put the substitution inside `bash -c` or a function.

## State
- Branch `master`, remote `origin` = https://github.com/bsommers/focker (PUBLIC). origin has `37ed603`. Local is 1
  ahead, NOT pushed: the Phase 5 planning commit. Untracked: this file only. No test processes running.
- v1.0 shipped (archived in .planning/milestones/). v2.0 Reach: Phase 5 PLANNED (GSD 05-CONTEXT/05-01-PLAN and
  docs/superpowers/plans/2026-10-20-fd-control-and-json.md, 4 tasks), awaiting user review. Phases 6-8 not planned.
- User decisions: push "1 - yes"; CRIU "2 - (a)": a capability-granted criu, focker unprivileged.
- `make ci` exit 0: 43 unit + 121 integration under the plain, ASan and UBSan builds.
- Authorizations: ask before each push. Enabling CI has NOT been authorized.

## Next
User reviews the Phase 5 plan. On "go": execute Tasks 1-4 natively, writing each test first (sb_json_str → inspect --json →
ps --json → release), `make ci`, update .planning + summary, then ask to push.
Resume check: `git log --oneline | head -1` → the Phase 5 planning commit, then `make ci`.
