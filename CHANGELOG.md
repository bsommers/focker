# Changelog

All notable changes to focker are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). The CLI contract
(commands, options, exit codes, `cutover:` transcript lines, `FOCKER_*`
variables, the control protocol in docs/USAGE.md) is the public API.

## [Unreleased]

### Added
- `focker release NAME FD...` closes held fds, for example to send a child EOF on the stdin its old parent held.
- `ps --json` and `inspect --json`. Keys are documented in docs/USAGE.md. Strings are always valid UTF-8.
- Test fixtures: `oldparent -I` (holds each child's stdin), `ticker -e` (exits 5 on stdin EOF).
- Cutovers take over loopback TCP connections (IPv4 and IPv6) between the old parent and a target, matched
  via `inet_diag`, as kind `tcp`, drained like unix sockets. The transcript shows the endpoints. Connections
  to other addresses and listening sockets are never taken.
- Test fixture: `oldparent -T4|-T6` connects each child's stdout over loopback TCP.
- docs/USE-CASES.md and `examples/usecases/`: four runnable walkthroughs that check their own claims
  (`make usecases`): outliving `timeout`, unwedging a hung orchestrator, replacing a log wrapper mid-job with
  `handover` (a Python new parent, `logwrap.py`), and pausing a writer for a snapshot.

### Changed
- A cutover waiting for its old parents no longer blocks the supervisor. Held fds keep draining, `info`,
  `members` and `inspect` are answered, and `cutover`, `release`, `stop` and `handover` are refused with
  `busy (a cutover is in progress)` (exit 1) until it ends. A client that disconnects mid-wait doesn't stop it.

### Fixed
- After serving a request, the supervisor could drain held fds through stale slot indices, and drained fds
  are blocking.

## [v1.0.0] - 2026-10-20

First release: reparent running Linux processes onto a new parent ("think
docker, but it's Meet the Fockers"), plus the v1.0 Hardening milestone.

### Added
- `focker run` starts a family under a `PR_SET_CHILD_SUBREAPER` supervisor, in the foreground or detached (`-d`), with `--linger`.
- `focker cutover` (alias `adopt`) moves named PIDs or `--from PPID` onto the supervisor:
  - ptrace freeze of every thread
  - takeover of the old parent's pipe ends and unix-socket peers with `pidfd_getfd`, draining them to logs
  - the old parent is signalled through a pidfd
  - verification, then a thaw that swallows signals sent during the cutover
  - `--freeze signal|none`, `--wait`, `--timeout` with full rollback, `--dry-run`
- `focker handover` (alias `meet-the-parents`) execs a new parent in place, keeping the PID, and passes `FOCKER_CHILDREN`, `FOCKER_FDS`, `FOCKER_NAME` and `FOCKER_STATE_DIR`.
- `ps [-a]`, `tree`, `inspect`, `logs [-f] [-p PID]`, `freeze`/`thaw`, `stop`, `rm`, `version`.
- Docs: README, docs/USAGE (with a limits table), ARCHITECTURE, HOWTO, LIMITATIONS.
- Build and test targets:
  - `make test`: 43 unit and 121 integration checks
  - `make asan`: separate ASan+LSan and UBSan builds, failing on any report
  - `make lint`: `-fanalyzer`, plus shellcheck
  - `make ci`, `make idle-cost`
- A GitHub Actions workflow in `ci/github-actions.yml`. It is disabled until it is moved into `.github/workflows/`.

### Changed
- Child lookup reads `/proc/PID/task/*/children` and falls back to the full `/proc` scan. An idle supervisor's CPU dropped from 11.3 ms/s to 52.6 µs/s with about 960 processes on the system.
- Cutover targets are pinned with a pidfd before validation and re-checked around the freeze.
- A cutover that would go past the limits is refused before anything is frozen: `--from` a parent with more than 64 children, or more than 128 held fds.

### Fixed
- A target that died during the thaw lost its exit status. The thaw now leaves an adopted child's exit to the reaper.
- A SIGSTOP sent during a cutover was swallowed. Job-control stops are kept now.
- Signals that were already pending before the freeze are reported, not silently merged.
- Held-fd slots are reused once their fds close. Before, a family could take only 128 fds in its lifetime.
- `handover` exits 1 with the reason when the exec fails. Before, it reported success.
- An fd leak in `run -d` when `fork` fails.

[v1.0.0]: https://github.com/bsommers/focker/releases/tag/v1.0.0
