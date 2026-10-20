---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# Coding Conventions

**Analysis Date:** 2026-10-20

## Style

- C11 + `_GNU_SOURCE`, 4-space indent, K&R braces, function braces on their own line, about 120 columns.
- Every `.c` file includes `"focker.h"` first, then system headers in alphabetical order.
- File-scope state is `static`. The only intentional global is `log_prefix`.
- Fixed-size buffers sized with `PATH_MAX` (plus slack where a suffix is appended, to keep `-Wformat-truncation` quiet) and `snprintf` everywhere. No `strcpy` of untrusted lengths.
- Every fd is opened with `O_CLOEXEC`/`SOCK_CLOEXEC`. The only fds that cross an exec are the ones handover deliberately clears.

## Comments

- A file header comment explains the module's purpose and the kernel fact it relies on.
- Comments explain *why*, especially non-obvious kernel behavior, e.g. "PTRACE_DETACH needs a stopped tracee". There is no commentary on obvious code.

## Error Handling

- Helpers return -1 and set errno (preserving errno across `close`).
- User-facing: one-line `focker: <what>: <strerror>` diagnostics on stderr. Never a crash or abort.
- Exit codes come from `FK_OK`/`FK_FAIL`/`FK_USAGE` (0/1/2). Usage errors go through `usage_error()`, which prints the command's usage.
- Processes may vanish at any moment, so every `/proc` read tolerates ESRCH/ENOENT and callers treat it as "gone".
- Multi-step operations that can fail half-way (cutover, handover) have an explicit rollback or recovery path.

## Output

- Data goes to stdout, diagnostics to stderr. A failed cutover's transcript goes to stderr.
- Transcript lines have stable, greppable prefixes (`cutover: adopted ...`, `plan: ...`), and tests depend on them.
- Lists are sorted (pids, names, fds) for deterministic output.

## CLI

- Every command has a `--help` with defaults and an example. Option parsing uses `getopt_long` with a leading `+` where a trailing command follows.
- Config precedence is flag > env (`FOCKER_*`) > default.
- Inputs are validated before any work: pid range, timeout bounds (0.1 to 3600), name charset, signal names.

## Docs

- Behavior changes must update `docs/USAGE.md` (and the README table if a command changes). Limits and gotchas go in `docs/LIMITATIONS.md`.

---

*Convention analysis: 2026-10-20*
