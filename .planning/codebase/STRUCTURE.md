---
last_mapped_commit: 3619015ca779dbbccd5fbe082d4cdf50902adba0
last_mapped_at: 2026-10-20T03:34:23Z
---

# Codebase Structure

**Analysis Date:** 2026-10-20

## Directory Layout

```
reparent/
├── Makefile            # build, test, demo, install
├── README.md           # pitch, quick start, command table
├── src/                # focker itself (≈2.9k lines of C)
│   ├── focker.h        # all shared types, limits, prototypes
│   ├── main.c          # CLI parsing, help, run/detach
│   ├── client.c        # control client, ps/tree/logs/rm/freeze/thaw
│   ├── supervisor.c    # subreaper loop, requests, handover
│   ├── cutover.c       # the cutover state machine
│   ├── procfs.c        # /proc readers
│   ├── unixdiag.c      # unix socket peer lookup via netlink
│   └── util.c          # sbuf, logging, paths, signals, pidfd wrappers
├── examples/           # fixtures for tests and the demo
│   ├── ticker.c        # child: prints ticks with its ppid; -d pdeathsig, -t threads
│   ├── oldparent.c     # parent: N tickers over pipes (or -s socketpairs), relays output
│   ├── newparent.c     # handover target: reads FOCKER_* env, relays, reaps
│   └── demo.sh         # narrated run → cutover → handover
├── tests/
│   └── run.sh          # 71 integration checks
├── docs/
│   ├── USAGE.md        # full reference (commands, env, files, protocol)
│   ├── ARCHITECTURE.md # kernel mechanics + design
│   ├── HOWTO.md        # recipes + troubleshooting
│   └── LIMITATIONS.md  # what it can't do and why
└── build/              # make output (gitignored)
```

## Where to Add New Code

- **New CLI command:** add a `cmd_*` parser and help string in `src/main.c` and dispatch it in `main()`. Put client-side logic in `src/client.c` and a new request verb in `handle_client()` in `src/supervisor.c`. Document it in `docs/USAGE.md` and the README command table.
- **New cutover behavior** (e.g. new fd kinds): `plan_fds()` and the steal loop in `src/cutover.c`. New fd kinds need a `kind` string that is also documented in the `FOCKER_FDS` contract.
- **New /proc reader:** `src/procfs.c`, with its prototype in `src/focker.h`.
- **New limit or constant:** `src/focker.h`.
- **Test fixtures:** `examples/`. Every `examples/*.c` is built automatically by the Makefile pattern rule.

## Naming Conventions

- Files: lowercase single words (`cutover.c`).
- Functions: `snake_case`. Command handlers are `cmd_*`, supervisor request handlers `do_*`, procfs readers `proc_*`, the netlink module `unixdiag_*`. A trailing `_` marks raw syscall wrappers (`pidfd_open_`).
- Constants: `MAX_*` limits, `FK_*` exit codes, `FREEZE_*` enum values.

## Special Directories

- `build/`: generated and gitignored.
- `$FOCKER_RUNTIME_DIR/NAME/` (outside the repo): runtime state per family.

---

*Structure analysis: 2026-10-20*
