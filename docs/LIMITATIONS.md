# focker limitations

These come from the kernel or from deliberate scope decisions. Each entry says why.

## Fundamental (kernel)

- **Only descendants.** Orphans go to the nearest *ancestor* subreaper, so focker must be above the old parent from the start. `cutover` cannot adopt an arbitrary existing process. `adopt-external` moves one anyway, by checkpointing it with criu and restoring it as the supervisor's child, within the limits below.
- **The old parent must die.** Linux never moves a child away from a live parent. If you can't kill the parent, you can't cut over. `--timeout` rolls back.
- **Every child of the old parent moves.** Its death orphans all of its children, not just the ones you named. focker reports them as `also adopted`. Name them, or use `--from`, so they are frozen and their fds are taken over too.
- **SIGKILL can't be swallowed.** A `PR_SET_PDEATHSIG=SIGKILL` child dies during the cutover, as does a child that the parent SIGKILLs on its way out.
- **A subreaper in between wins.** If any process between focker and the old parent has `PR_SET_CHILD_SUBREAPER`, the orphan goes there. focker reports this and does not retry.
- **PID namespaces.** A namespace's init is the last subreaper for that namespace, so focker must be *inside* the namespace with the processes it adopts.

## By design or not yet handled

- **Linked fds only.** focker takes over pipes (opposite ends of the same pipe), unix sockets (peer of the child's socket), and loopback TCP connections (`127.0.0.0/8` or `::1`, both ends on this host, matched via `inet_diag`). It does **not** match TCP connections to other addresses (even the host's own non-loopback IPs), listening sockets, UDP sockets, ptys, eventfds, or memfds. Those survive the parent's death on their own unless the parent was the only holder of the far end.
- **Held unix and TCP peers are drained, never written.** A child that waits to *read* from a socket the old parent used to write to will wait forever, unless a new parent takes over writing after a `handover`.
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
- **Limits:** 64 targets per cutover (`--from` a parent with more children is refused; name up to 64 of them instead), 128 held fds per family (a cutover that would go past it is refused before anything is frozen; slots of closed fds are reused), and 256 remembered members per family (the oldest dead members are forgotten first). The full table is in [USAGE](USAGE.md#limits).
- **adopt-external moves what criu can move.** It refuses (before touching anything) a job with an established TCP connection, a unix socket or a pipe to a process outside the job, a process that leads a session (a shell), another user's process, or more than 64 processes, 16 roots or 16 pipes between roots. Other things criu cannot checkpoint are caught by the trial dump.
- **adopt-external needs criu with `CAP_CHECKPOINT_RESTORE`.** Run `sudo setcap cap_checkpoint_restore+eip "$(command -v criu)"` once. focker stays unprivileged. Under Yama `ptrace_scope` 1, criu may attach only to its own descendants, so `kernel.yama.ptrace_scope=0` is needed too (focker names the sysctl); 2 or 3 rule it out.
- **adopt-external kills the original.** Between `killed the originals (point of no return)` and the restore, the job exists only as images. A failed restore keeps them, with the criu command to retry by hand. The PIDs must still be free when it restores, which they are unless something takes them in those milliseconds.
- **One terminal job per family.** A family holds one terminal. A terminal job joins only a detached family that has none yet; start a new family for each.
- **A moved job gets a new process group.** The shell in the old terminal may still list the old group, and signals every group it lists when its terminal closes, so every process of the job in that group moves to a new one. Processes in groups of their own keep them.
- **adopt-external is synchronous.** criu runs inside the supervisor's request (up to 60 s per run), so the family answers no other request and drains no pipes until the move ends.
- **Linux only**, 5.6 or later (`pidfd_getfd`, `pidfd_send_signal`, `pidfd_open`).
