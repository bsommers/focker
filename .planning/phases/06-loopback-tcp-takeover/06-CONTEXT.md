# Phase 6: Loopback TCP takeover - Context

**Gathered:** 2026-10-20
**Status:** Ready for planning

<domain>
## Phase Boundary

FD-01: an established loopback TCP connection between an old parent and a target is matched through
`NETLINK_SOCK_DIAG` / inet_diag and taken over like a unix socket (drained to a log). IPv4 and IPv6. No
other fd kinds, and nothing that leaves the host.

</domain>

<decisions>
## Implementation Decisions

- **D-01:** New `src/inetdiag.c`, mirroring `unixdiag.c`: `inetdiag_load()` dumps TCP sockets for AF_INET
  and AF_INET6 (`inet_diag_req_v2`, `idiag_states = 1 << TCP_ESTABLISHED`), keeps only loopback ones, and
  pairs socket A with socket B when A's local endpoint is B's remote endpoint and the other way round.
  `inetdiag_peer(ino)` returns the peer inode or 0. `inetdiag_endpoints(ino, buf, len)` formats
  `127.0.0.1:41234 <-> 127.0.0.1:38211` for transcripts.
- **D-02:** Loopback means `127.0.0.0/8`, `::1`, or IPv4-mapped `::ffff:127.0.0.0/104`. Anything else is
  never paired, so a socket to another host is never taken over. Listening and non-established sockets
  aren't dumped at all.
- **D-03:** `plan_fds` tries the unix peer first, then the TCP peer. The new kind is `tcp`, drained like
  `unix`. The `plan:` and `took over` lines add the endpoints in parentheses after the existing text, so
  current greps still match.
- **D-04:** If the inet_diag dump fails, the cutover warns like the unix case
  (`cannot read TCP socket peers (...); TCP connections will not be taken over`) and continues.
- **D-05:** Fixture: `oldparent -T4|-T6` connects each child's stdout to the parent over a loopback TCP
  connection (listen on port 0, connect, accept) instead of a pipe. IPv6 tests are skipped when `::1`
  can't be bound.
- **D-06:** Unit tests cover the loopback classifier and live pairs (v4, v6 when available, and a
  listening socket that must have no peer).

</decisions>
