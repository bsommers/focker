# focker limitations

These come from the kernel or from deliberate scope decisions. Each entry says why.

## Fundamental (kernel)

- **Only descendants.** Orphans go to the nearest *ancestor* subreaper, so focker must be above the old parent from the start. There is no way to adopt an arbitrary existing process. Use CRIU for that ([HOWTO](HOWTO.md#processes-not-started-under-focker)).
- **The old parent must die.** Linux never moves a child away from a live parent. If you can't kill the parent, you can't cut over. `--timeout` rolls back.
- **Every child of the old parent moves.** Its death orphans all of its children, not just the ones you named. focker reports them as `also adopted`. Name them, or use `--from`, so they are frozen and their fds are taken over too.
- **SIGKILL can't be swallowed.** A `PR_SET_PDEATHSIG=SIGKILL` child dies during the cutover, as does a child that the parent SIGKILLs on its way out.
- **A subreaper in between wins.** If any process between focker and the old parent has `PR_SET_CHILD_SUBREAPER`, the orphan goes there. focker reports this and does not retry.
- **PID namespaces.** A namespace's init is the last subreaper for that namespace, so focker must be *inside* the namespace with the processes it adopts.

## By design or not yet handled

- **Linked fds only.** focker takes over pipes (opposite ends of the same pipe) and unix sockets (peer of the child's socket). It does **not** match TCP/UDP sockets (both ends of a loopback TCP connection could be matched via `inet_diag`; not implemented), ptys, eventfds, or memfds. Those survive the parent's death on their own unless the parent was the only holder of the far end.
- **Held unix peers are drained, never written.** A child that waits to *read* from a socket the old parent used to write to will wait forever, unless a new parent takes over writing after a `handover`.
- **Process group and session don't change.** The child keeps the old parent's pgrp, session and controlling terminal. Job control from the original shell still reaches it.
- **cgroup doesn't change.** Move it yourself with `echo PID > /sys/fs/cgroup/.../cgroup.procs` if you need to.
- **Threads cloned mid-freeze.** The thread list is re-read until it is stable (up to 16 rounds). A process that creates threads faster than that can escape the freeze. The limit is 256 threads per frozen process.
- **One tracer at a time.** A process already under gdb or strace can't be ptrace-frozen. Use `--freeze signal` or `none`, and accept that pending signals get delivered.
- **Yama `ptrace_scope` 2 or 3** blocks ptrace and `pidfd_getfd` for non-root users. Only `--freeze signal --no-steal` still works.
- **`--freeze signal` is observable.** The parent may get `SIGCHLD` with `CLD_STOPPED`, and signals sent during the window are delivered on thaw. If the parent's exit leaves the stopped child's process group orphaned (focker's own layouts avoid this, but a child that moved itself into an unusual pgrp/session setup may not), the kernel sends it `SIGHUP`+`SIGCONT` ([ARCHITECTURE §5.2](ARCHITECTURE.md#52-orphaned-process-groups-and-sigstop)).
- **Swallow window.** Signals that arrive between freeze and thaw are swallowed (except CHLD, CONT, WINCH, URG, and the job-control stops, which are kept). A legitimate signal sent to the child during those milliseconds is dropped too. Use `--no-suppress` to deliver everything.
- **Already-pending signals can't be swallowed.** If a standard signal is pending (usually because the child blocks it) when the freeze starts, a copy sent during the cutover merges into it, and the kernel keeps no trace of the second send. focker names such signals in the transcript and leaves them pending.
- **Handover is one-way.** After `handover`, focker no longer controls the family. The new program owns everything.
- **Requests are serialized.** One cutover at a time per family, and while one is running the supervisor doesn't drain other pipes. A cutover usually takes milliseconds, and at most `--timeout`.
- **Limits:** 64 targets per cutover, 128 held fds and 256 remembered members per family (the oldest dead members are forgotten first).
- **Linux only**, 5.6 or later (`pidfd_getfd`, `pidfd_send_signal`, `pidfd_open`).
