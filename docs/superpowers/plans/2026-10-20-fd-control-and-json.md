# Phase 5 Fd Control and JSON Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `focker release NAME FD...` closes single held fds, and `ps --json` / `inspect --json` give scripts machine-readable state.

**Architecture:** One JSON string escaper (`sb_json_str`) in `src/util.c` is shared by the supervisor, which builds `inspect --json` because it owns the member table, and by the client, which builds `ps --json` from `info` replies and state-dir files. `release` is a new control request handled by the supervisor. It closes the fd and its log and frees the slot, so later cutovers reuse it.

**Tech Stack:** C11 + glibc, the existing `tests/unit.c` and `tests/run.sh` harnesses, and `python3 -m json.tool` for validation in tests.

**Spec:** `.planning/REQUIREMENTS.md` (FD-02, OUT-01), `.planning/ROADMAP.md` Phase 5, `.planning/phases/05-fd-control-and-json/05-CONTEXT.md` (decisions D-01..D-07, schema in D-05)

## Global Constraints

- FD-02: "`focker release NAME FD` closes one held fd (for example to send EOF to a child's stdin). `inspect` no longer lists it, its drain log is closed, and releasing an unknown fd exits 1"
- OUT-01: "`ps --json` and `inspect --json` print one JSON document (stable keys, documented in USAGE)"
- Existing text output, transcript lines and exit codes (0 / 1 runtime / 2 usage) don't change.
- No new dependencies. python3 is a test-only dependency: tests skip without it.
- `make ci` must pass (lint + test + asan).

## Review Focus

- A cmdline with `"`, `\`, a newline, a tab or invalid UTF-8 (`\xff`): the JSON must still parse. (Task 1 unit tests, plus a Task 2 integration test with a `$'a"b\\c\xff'` argument.)
- `release` of an fd that was already closed by EOF draining: it's "no such fd", exit 1, not a crash or a double close. (Task 4.)
- `release` of several fds where one is unknown: the others are still released, and the exit code is 1. (Task 4.)
- `ps --json` when a family's socket is gone but its dir remains (`-a`): `running:false`, `pid` from the pid file, `children:null`. (Task 3.)
- `inspect --json` with no members besides the first command, and no held fds: `"held_fds":[]`, never a missing key. (Task 2.)

---

### Task 1: JSON string escaper

**Files:**
- Modify: `src/util.c`, `src/focker.h`
- Test: `tests/unit.c` (`test_json_str`)

**Interfaces:**
- Produces: `void sb_json_str(struct sbuf *b, const char *s);` appends `s` as a quoted JSON string. `NULL` appends `null`.

- [x] **Step 1: Write the failing tests** (`tests/unit.c`, new section "json strings"). Each one renders into a fresh `sbuf` and `strcmp`s the result:
  - `plain`: `abc` → `"abc"`
  - `quotes and backslash`: `a"b\c` → `"a\"b\\c"`
  - `control chars`: `"\n\t\x01"` → `"\n\t\u0001"` (`\n` and `\t` as short escapes, the rest as `\u00XX`)
  - `utf-8 passes through`: `"h\xc3\xa9"` → `"h\xc3\xa9"`
  - `invalid utf-8 is replaced`: `"a\xffb"` → `"a�b"`, and the truncated sequence `"\xc3"` → `"�"`
  - `NULL`: `null`
- [x] **Step 2:** `make unit`: it doesn't compile (`sb_json_str` is undeclared).
- [x] **Step 3: Implement** `sb_json_str` in `src/util.c`. Validate each UTF-8 sequence's lead byte, length and continuation bytes, and reject overlongs and surrogates. Emit `�` for each invalid byte.
- [x] **Step 4:** `make unit`: all pass.
- [x] **Step 5:** Commit: `util: sb_json_str, a JSON string escaper that always emits valid UTF-8`

### Task 2: `inspect --json`

**Files:**
- Modify: `src/supervisor.c` (`do_inspect`; request token `json=1`), `src/main.c` (`inspect` option parsing, `--json`), `docs/USAGE.md` (`focker tree / inspect` section: the schema from 05-CONTEXT D-05)
- Test: `tests/run.sh` (new section `json`)

**Interfaces:**
- Consumes: `sb_json_str` (Task 1).
- Produces: the control request `inspect json=1`, whose reply body is the JSON document. `status` uses the same text as the table (`running`, `exited 3`, `killed by SIGTERM`). `exit_code` is an int or null. `signal` is a name like `"SIGTERM"` or null. `from` and `log` are null when absent.

- [x] **Step 1: Write the failing tests** (`tests/run.sh`, section `json`, skipped without python3 as in the Ctrl-C test). Start `jsonfam` with `"$B/oldparent" 1 -- -i 50` plus a dummy argument `$'a"b\\c\xff'` given to the ticker (ticker ignores extra args after getopt, so put it after `--`). Cut over the ticker. Checks:
  - `inspect --json is valid JSON`: `"$F" inspect --json jsonfam | python3 -m json.tool >/dev/null`
  - `inspect --json lists the adopted member`: `python3 -c` loads it and asserts some member has `pid == T`, `origin == "adopted"`, `alive is True`
  - `inspect --json lists the held fd`: `held_fds[0]` has `kind == "pipe-r"`, `child == T`, `child_fd == 1`, `log` ending with `adopted-T-fd1.log`
  - `inspect --json survives odd cmdline bytes`: covered by the first check, since the ticker's cmdline contains the odd argument
  - `inspect --json on a missing family exits 1`
- [x] **Step 2:** `make test`: the new checks fail (`--json` is an unknown option, exit 2).
- [x] **Step 3: Implement.** In `main.c`, `inspect` accepts `--json` and sends `inspect` + `json=1`. In `supervisor.c`, `do_inspect` takes `bool json` and builds the D-05 object with `sb_json_str` for every string. Write the schema into USAGE.
- [x] **Step 4:** `make test`: everything passes.
- [x] **Step 5:** Commit: `inspect --json (OUT-01)`

### Task 3: `ps --json`

**Files:**
- Modify: `src/client.c` (`cmd_ps(bool all, bool json)`), `src/main.c`, `src/focker.h` (prototype), `docs/USAGE.md` (`focker ps`)
- Test: `tests/run.sh` (section `json`)

**Interfaces:**
- Consumes: `sb_json_str`, `client_info`, `read_leaf`.
- Produces: `int cmd_ps(bool all, bool json);` For each family it emits `{"name","pid","running","status","children","cmd"}`. When the family isn't running, `status` is the `exit` file's text, or `"gone (no exit record)"`.

- [x] **Step 1: Write the failing tests:**
  - `ps --json is valid JSON`, with `jsonfam` running
  - `ps --json shows the running family`: `families` has `name == "jsonfam"`, `running is True`, `children == 1`
  - `ps -a --json includes stopped families`: after `stop_family jsonfam`, the entry has `running is False` and `children is None`, and `status` is a non-empty string
  - `ps --json without -a omits stopped families`
- [x] **Step 2:** `make test`: they fail.
- [x] **Step 3: Implement** with a single `struct sbuf` printed at the end, so the output is one document even if a family vanishes mid-listing.
- [x] **Step 4:** `make test`: everything passes.
- [x] **Step 5:** Commit: `ps --json (OUT-01)`

### Task 4: `focker release`

**Files:**
- Modify: `examples/ticker.c` (`-e`), `examples/oldparent.c` (`-I`), `src/supervisor.c` (`do_release`), `src/main.c` (`release` command + usage text), `docs/USAGE.md` (new `focker release` section), `README.md` (command table), `docs/HOWTO.md` (the "Close what you don't need" note now points to `release`), `docs/LIMITATIONS.md` (remove "a way to release a single held fd" if it's listed; the CONCERNS.md missing-feature line too)
- Test: `tests/run.sh` (new section `release`)

**Interfaces:**
- Produces: the control request `release FD...`, with one reply line per fd: `release: closed KIND fd N (child PID fd M)` or `release: focker holds no fd N`. rc is 1 if any fd was unknown. The CLI is `focker release NAME FD...`. A non-numeric FD or a missing FD is a usage error (exit 2).
- Fixtures: `ticker -e` exits with status 5 when stdin reaches EOF (read loop in a thread). `oldparent -I` gives each child a stdin pipe, keeps the write end, and never writes to it.

- [x] **Step 1: Write the failing tests:**
  - Start `rel` with `start --linger rel "$B/oldparent" -I 1 -- -i 50 -e`, so `inspect` still answers after the ticker exits, and cut over the ticker. The transcript has `took over pipe-w` and `took over pipe-r`.
  - `release closes a held stdin`: take FD from the `pipe-w` row of `inspect`. `"$F" release rel "$FD"` exits 0 and prints `release: closed pipe-w fd $FD`.
  - `the child sees EOF`: `wait_for` until `inspect rel` shows `$T` with `exited 5`.
  - `inspect stops listing a released fd`: there is no `pipe-w` row.
  - `releasing a drained fd closes its log`: release the `pipe-r` FD, and `lsof`-free proof that its log is closed: `/proc/$SUP/fd` no longer has a link to `adopted-$T-fd1.log`.
  - `releasing an unknown fd exits 1`: `release rel 9999` → exit 1, `focker holds no fd 9999`.
  - `release with one unknown fd still releases the others`: on a fresh family (`rel2`, two held fds), `release rel2 $FD 9999` exits 1 and `inspect` no longer lists `$FD`.
  - `release needs an fd`: `release rel` → exit 2. `release rel x` → exit 2.
- [x] **Step 2:** `make test`: they fail (unknown command, exit 2).
- [x] **Step 3: Implement** the fixtures, then `do_release(char **args, int nargs, struct sbuf *out)` in `src/supervisor.c`. Match `stolen[i].fd == N`, close `fd` and `logfd`, set both to -1, and `logmsg("released %s fd %d (child %d fd %d)", ...)`. Add `cmd_release` parsing in `main.c` with `getopt_long("+h")`, matching the other commands.
- [x] **Step 4:** `make ci`: exit 0.
- [x] **Step 5:** Commit: `focker release NAME FD... (FD-02)`. Update `.planning` (REQUIREMENTS FD-02/OUT-01 complete, ROADMAP Phase 5, STATE, and `05-01-SUMMARY.md`) in the same commit.

## Outcome (2026-10-20)

All steps done in four commits (`sb_json_str`, `inspect --json`, `ps --json`, `focker release`). Deviations:
- `cmd_inspect` was first called with an extra `argv + 1`, which ate `--json`. The red tests caught it.
- Found and fixed while doing Task 4: after serving a request, the poll loop drained slots through stale
  indices, and drained fds are blocking. It now skips draining in that round.
`make ci` passes: 53 unit + 144 integration checks under the plain, ASan and UBSan builds.
