# Phase 6 Loopback TCP Takeover Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A child connected to its old parent by a loopback TCP connection keeps that connection through a cutover, with its output drained to a log like a unix socket's.

**Architecture:** A new `src/inetdiag.c`, mirroring `unixdiag.c`, dumps established TCP sockets over `NETLINK_SOCK_DIAG` for AF_INET and AF_INET6. It keeps the loopback ones and pairs sockets whose local and remote endpoints mirror each other. `plan_fds` in `cutover.c` falls back to this pairing when a socket has no unix peer, and plans a drained `tcp` takeover.

**Tech Stack:** C11, Linux UAPI (`linux/inet_diag.h`, `linux/sock_diag.h`), the existing unit and integration harnesses.

**Spec:** `.planning/REQUIREMENTS.md` (FD-01), `.planning/ROADMAP.md` Phase 6, `.planning/phases/06-loopback-tcp-takeover/06-CONTEXT.md` (D-01..D-06)

## Global Constraints

- FD-01: "Loopback TCP connections between the old parent and a target are matched via `NETLINK_SOCK_DIAG`/`inet_diag` (IPv4 and IPv6) and taken over like unix sockets; `--dry-run` shows them as `tcp`"
- Roadmap: "Non-loopback and listening sockets are never taken"
- Existing transcript lines keep their shape. TCP adds ` (LOCAL <-> REMOTE)` at the end of the line.
- No privileges: a same-uid inet_diag dump is all it needs. No new dependencies. `make ci` must pass.

## Review Focus

- **Two connections between the same pair of processes:** each child socket must pair with exactly its own peer, never the other connection. (Task 1 live test with two connections. Task 2 fixture `oldparent -T4 2`.)
- **A connection to a non-loopback address:** never paired. (Task 1 classifier test on `10.0.0.1`, `192.168.1.5`, `::ffff:10.0.0.1`, `fe80::1`.)
- **A listening socket the child inherited:** no peer, so nothing is taken. (Task 1 live test.)
- **IPv6 unavailable (`::1` can't be bound):** the v6 tests are skipped, not failed. (Tasks 1 and 2.)
- **A dump with thousands of sockets on the host:** the result is truncated by nothing but memory, and it's a single dump per cutover, like unixdiag. (Task 1 reuses the realloc-grown table from unixdiag.)

---

### Task 1: `src/inetdiag.c`

**Files:**
- Create: `src/inetdiag.c`
- Modify: `src/focker.h`, `Makefile` (link `build/inetdiag.o` into `build/unit`)
- Test: `tests/unit.c` (sections "loopback classifier" and "live tcp pairs")

**Interfaces:**
- Produces:
  - `int inetdiag_load(void);` returns 0, or -1 with errno.
  - `unsigned long inetdiag_peer(unsigned long ino);` returns 0 if the socket isn't a loopback established TCP socket with a local peer.
  - `int inetdiag_endpoints(unsigned long ino, char *buf, size_t len);` writes `"127.0.0.1:41234 <-> 127.0.0.1:38211"` (IPv6 as `[::1]:port`), and returns 0 or -1.
  - `bool inet_is_loopback(int family, const void *addr);`, exported for the unit test.

- [ ] **Step 1: Write the failing tests:**
  - `inet_is_loopback`: true for `127.0.0.1`, `127.255.0.9`, `::1` and `::ffff:127.0.0.1`. False for `10.0.0.1`, `192.168.1.5`, `::`, `::ffff:10.0.0.1` and `fe80::1`.
  - Live v4: listen on `127.0.0.1:0`, connect twice, and accept both, giving client sockets c1, c2 and server sockets s1, s2. After `inetdiag_load() == 0`: `inetdiag_peer(ino(c1)) == ino(s1)` and the reverse, `inetdiag_peer(ino(c2)) == ino(s2)`, and `inetdiag_peer(ino(listener)) == 0`. `inetdiag_endpoints(ino(c1))` starts with `127.0.0.1:` and contains ` <-> 127.0.0.1:`.
  - Live v6: the same on `::1`, skipped (printed `skip`) if `bind` fails. The endpoints start with `[::1]:`.
- [ ] **Step 2:** `make unit`: it doesn't compile (undeclared functions).
- [ ] **Step 3: Implement.** Send one `SOCK_DIAG_BY_FAMILY` dump per family with `struct inet_diag_req_v2`: `sdiag_protocol = IPPROTO_TCP`, `idiag_states = 1 << 1` (TCP_ESTABLISHED, from `linux/inet_diag.h`'s state numbering). Each `inet_diag_msg` gives `idiag_inode` and `id.idiag_src/dst/sport/dport`; ports are network order. Keep loopback-only entries in a table `{ino, family, local addr+port, remote addr+port}`. Pair entries with an O(n²) scan after the dump. The tables are small: only loopback established sockets are kept.
- [ ] **Step 4:** `make unit`: all pass. `make lint`: clean.
- [ ] **Step 5:** Commit: `inetdiag: pair loopback TCP sockets via NETLINK_SOCK_DIAG`

### Task 2: TCP takeover in the cutover

**Files:**
- Modify: `examples/oldparent.c` (`-T4`, `-T6`), `src/cutover.c` (`plan_fds`, `plan_line`, the load call next to `unixdiag_load`), `docs/USAGE.md` (`KIND` values: add `tcp`), `docs/ARCHITECTURE.md` (§4.1 fd plan: a TCP bullet), `docs/LIMITATIONS.md` ("Linked fds only": TCP loopback is now matched, non-loopback still isn't), `CHANGELOG.md` (Unreleased, Added)
- Test: `tests/run.sh` (new section `cutover (tcp)`)

**Interfaces:**
- Consumes: `inetdiag_load`, `inetdiag_peer`, `inetdiag_endpoints` (Task 1).
- Produces: plan kind `"tcp"` (`drain = true`). `FOCKER_FDS` entries use `tcp` as KIND. Fixture `oldparent -T4|-T6 [N] [-- TICKER-ARGS]`: each child's stdout is the client end of a loopback TCP connection whose accepted end the parent reads.

- [ ] **Step 1: Write the failing tests** (`tests/run.sh`, section `cutover (tcp)`):
  - Start `tcp4` with `"$B/oldparent" -T4 2 -- -i 50`, `T` = the first ticker.
  - `dry run plans the tcp takeover`: `--dry-run` output matches `plan: take over tcp fd [0-9]+ of $OLD (child $T fd 1).*\(127\.0\.0\.1:[0-9]+ <-> 127\.0\.0\.1:[0-9]+\)`.
  - `tcp cutover succeeds`: `--from $OLD` exits 0, and the transcript has two `took over tcp` lines.
  - `the tcp child keeps writing after the cutover`: `wait_for grep -q "ppid=$SUP" "$FOCKER_RUNTIME_DIR/tcp4/adopted-$T-fd1.log"`.
  - `the tcp child is alive`: `alive $T`.
  - `FOCKER_FDS-style kind in inspect`: `inspect tcp4` has a ` tcp ` row.
  - `tcp6`: the same with `-T6` and `\[::1\]`. If `python3 -c 'import socket; s=socket.socket(socket.AF_INET6); s.bind(("::1",0))'` fails, print `skip`.
  - `a listening socket is not taken`: the old parent is `python3 -c` and listens on `127.0.0.1:0`. It starts `$B/ticker -i 50` with `subprocess.Popen(..., pass_fds=[listener.fileno()], stdout=DEVNULL)` and then `wait()`s, so parent and child share one listening socket. `--dry-run` on the ticker prints no `tcp` line. Skipped without python3.
- [ ] **Step 2:** `make test`: they fail (`oldparent: unknown -T4`, then no `tcp` plan lines).
- [ ] **Step 3: Implement.** Fixture first. In `plan_fds`, for `kind == 's'`: `linked = unixdiag_peer(p) == c` and kind `unix`, else `inetdiag_peer(p) == c` and kind `tcp`. Carry the endpoints string in `struct plan_fd` (`char ends[96]`), and have `plan_line` append ` (ENDS)` when it's set. Call `inetdiag_load()` right after `unixdiag_load()`, with the D-04 warning on failure.
- [ ] **Step 4:** `make ci`: exit 0.
- [ ] **Step 5:** Commit: `cutover: take over loopback TCP connections (FD-01); Phase 6 complete`, together with the `.planning` updates (REQUIREMENTS FD-01, ROADMAP, STATE, `06-01-SUMMARY.md`).
