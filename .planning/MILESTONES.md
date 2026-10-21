# Milestones

## v1.0 Hardening (Shipped: 2026-10-20)

**Phases completed:** 4 phases (1-4), 5 plans
**Archive:** `.planning/milestones/v1.0-ROADMAP.md`, `.planning/milestones/v1.0-REQUIREMENTS.md`

**Key accomplishments:**
- Cutover edge cases fixed: an adopted child's exit status kept through thaw, job-control stops kept, targets pinned by pidfd, already-pending signals reported (REL-01..04)
- Integration suite 71 → 121 checks, plus 43 parser unit checks. Limit bugs fixed: `--from` over 64 children, held-fd slot reuse, refusing a cutover past 128 fds up front
- `make asan` (separate ASan+LSan and UBSan builds), `make lint` (`-fanalyzer`, shellcheck), `make ci`. GitHub workflow written but disabled (`ci/`)
- Child lookup through `/proc/PID/task/*/children`: idle supervisor CPU 11.3 ms/s → 52.6 µs/s
- Follow-ups: `handover` reports a failed exec (exit 1), and the test cleanup kills by session

---
