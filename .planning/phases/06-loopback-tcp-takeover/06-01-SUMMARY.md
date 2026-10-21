---
phase: 06-loopback-tcp-takeover
plan: 01
subsystem: cutover
tags: [fds, tcp, sock_diag]
requires: [05-01]
provides: [inetdiag_load, inetdiag_peer, inetdiag_endpoints, inet_is_loopback, fd kind tcp, oldparent -T4/-T6]
affects: [07-async-cutover]
key-files:
  created: [src/inetdiag.c]
  modified: [src/cutover.c, src/focker.h, src/main.c, Makefile, examples/oldparent.c, examples/newparent.c, tests/unit.c, tests/run.sh, docs/USAGE.md, docs/ARCHITECTURE.md, docs/LIMITATIONS.md, CHANGELOG.md]
key-decisions:
  - "addresses compared in IPv6 form so a dual-stack socket pairs with a plain IPv4 one"
  - "unix peer tried first, then TCP; endpoints appended in parentheses so existing greps still match"
requirements-completed: [FD-01]
completed: 2026-10-20
---

# Phase 6 Plan 01: Loopback TCP takeover Summary

**Cutovers take over loopback TCP connections (IPv4 and IPv6) as kind `tcp`, drained like unix sockets. Steps are in docs/superpowers/plans/2026-10-20-loopback-tcp.md.**

## Accomplishments
- `src/inetdiag.c`: one established-TCP `inet_diag` dump per family, loopback-only table, mirror-endpoint pairing. 31 unit checks: the classifier (`127.x`, `::1`, `::ffff:127.x` yes; `10.x`, `192.168.x`, `::`, `::ffff:10.x`, `fe80::1` no), two live connections per family each paired with its own peer, a listener with no peer, a dual-stack server paired with an IPv4 client.
- `plan_fds` falls back to the TCP peer; `plan:`/`took over` lines end with `(L <-> R)`. D-04 warning when the dump fails.
- `oldparent -T4|-T6`; `newparent` relays `tcp` fds. 13 integration checks: v4 and v6 cutovers of two connections, drained output, `inspect` row, and a shared listening socket that is not taken.

## Deviations / found along the way
- `-fanalyzer` flagged an fd stored through an out-pointer in the unit fixture; fds are now kept in locals until success.

## Verification
- `make ci` exit 0: 84 unit + 157 integration checks under the plain, ASan and UBSan builds, no sanitizer reports.
