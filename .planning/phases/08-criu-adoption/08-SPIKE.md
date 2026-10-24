# Phase 8 spike: criu 4.2 unprivileged, new pty, pipelines (2026-10-21)

Machine: Linux 7.0, criu 4.2 with `cap_checkpoint_restore=eip`, Yama `ptrace_scope=1`. Throwaway scripts; the
"terminal A" was `bash --norc -i` on a Python pty, not under the restorer.

## 1. Privilege
`criu dump --unprivileged --shell-job -t PID` of a job in another terminal FAILS with only
`cap_checkpoint_restore`: `Warn (compel/src/lib/infect.c:131): Unable to interrupt task: PID (Operation not
permitted)`. Yama scope 1 only lets an ancestor ptrace. It works once the target allows it (the rest of the spike
used a target that called `prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY)` before exec). For real jobs criu needs
`cap_sys_ptrace` as well, or Yama scope 0. **User decision needed.** Restore needs no extra privilege.

## 2. Terminal
`--external tty[...]` is not needed at dump: `--shell-job` treats the job's terminal as external.
`--inherit-fd fd[N]:tty[8836:1c]` at restore FAILS: `tty: No task found with sid <terminal A's shell>`.
What works: run `criu restore --shell-job` with its fds 0, 1 and 2 on the new pty slave. The restored tasks join
criu's session and get the new pty as their controlling terminal (`/proc/PID/stat` tty_nr = the new pty).

## 3. Session, parent, Ctrl-C
The restorer: `setsid()`, open a pty, `ioctl(slave, TIOCSCTTY, 0)`, `prctl(PR_SET_CHILD_SUBREAPER, 1)` (AFTER any
fork: the flag is not inherited), then fork+exec `criu restore --unprivileged --shell-job --restore-detached -D DIR
--pidfile FILE`. Result: same PID, ppid = the restorer, sid = the restorer, ctty = the new pty, output arrives on
the master. `tcsetpgrp(slave, job_pgid)` from the restorer succeeds (SIGTTOU ignored), and writing 0x03 to the
master killed the job with SIGINT.
`--pidfile FILE` fails with `File exists` if FILE exists: remove it first. Images can be restored again after a
failed or finished restore (restored the same images 3 times).

## 4. Restore stopped
`criu restore ... --leave-stopped` leaves the restored tasks in state T; SIGCONT starts them.

## 5. Pipelines
- Stop the group (`kill(-pgid, SIGSTOP)`), then `criu dump --leave-stopped -t ROOT` once per root. Each succeeds
  although the pipe's other end is outside that root's tree.
- The buffered pipe data is saved once, in the dump of the root holding the read end (`pipes-data.img`).
- Restore each root with `--inherit-fd fd[N]:pipe:[INO]`, giving the writer root the write end and the reader root
  the read end of ONE fresh pipe. INO is the id from `readlink /proc/PID/fd/K` (`pipe:[INO]`, = the image's
  `pipe_id`; `stat -L` agreed here, but use readlink as focker's `proc_fds` does).
- **criu does not refill buffered data into an inherited pipe** (13893 bytes lost, 3 of 3 runs). Refilling it
  from the image before the restores gives exactly-once delivery (3000 of 3000 lines, sum checked).
  `pipes-data.img` layout: 8 bytes of magic, then entries of: u32 little-endian length, a protobuf
  `PipeDataEntry` of that length (field 1 `pipe_id` varint, field 2 `bytes` varint, field 3 `size` varint), then
  `bytes` raw bytes. Set the new pipe's size to `size` with `F_SETPIPE_SZ` before writing.
- The second and later roots are restored into criu's process group (the restorer's), not the job's. The parent
  fixes it with `setpgid(root, job_pgid)` while they are stopped (allowed: restored tasks never exec'd). Restore
  the root whose pid == pgid first, so the group exists.

## 6. Refusals (first `Error` line of the dump log; show it without the `Error (file:line): ` prefix)
- Established TCP: `inet: Connected TCP socket, consider using --tcp-established option.`
- Unix socket to a process outside the tree: `unix: External socket is used. Consider using --ext-unix-sk option.`
- Missing `--shell-job`: `The root process PID is not a session leader. Consider using --shell-job option`
- No ptrace permission: `Unable to interrupt task: PID (Operation not permitted)` (a Warn line; then
  `Unable to detach` and `Dumping FAILED` errors).

## Plan changes (rulings)
- `criu_dump_argv`: no `--external tty`. `criu_restore_argv`: no tty `--inherit-fd`; pipes only; `--pidfile`.
- The supervisor runs each criu restore with fds 0-2 on the pty slave (or /dev/null when the job has no terminal),
  refills buffered pipe data from the reader's `pipes-data.img`, restores the pgid leader first, and `setpgid`s the
  other roots before SIGCONT.
- The capability probe also has to cover ptrace (see 1).
