#!/usr/bin/env python3
"""Run a command on a fresh pty, the way a terminal would, for the tests.

usage: ptyrun.py [--rows R --cols C] [--send TEXT@SECONDS]... [--stty-check] [--timeout S] -- CMD...

Each --send writes TEXT (Python escapes: \\x03, \\x10\\x11) to the terminal SECONDS after
the start. Everything CMD prints is copied to stdout. Exits with CMD's exit code
(128+N if killed by signal N); 99 if --stty-check finds the terminal's settings
changed; 124 if CMD outlives --timeout (default 10) and is killed.
"""
import argparse
import codecs
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import termios
import time


def parse():
    p = argparse.ArgumentParser(prog="ptyrun.py")
    p.add_argument("--rows", type=int)
    p.add_argument("--cols", type=int)
    p.add_argument("--send", action="append", default=[], metavar="TEXT@SECONDS")
    p.add_argument("--stty-check", action="store_true")
    p.add_argument("--timeout", type=float, default=10)
    p.add_argument("cmd", nargs=argparse.REMAINDER)
    a = p.parse_args()
    if a.cmd and a.cmd[0] == "--":
        a.cmd = a.cmd[1:]
    if not a.cmd:
        p.error("missing command")
    sends = []
    for s in a.send:
        text, sep, when = s.rpartition("@")
        if not sep:
            p.error("--send wants TEXT@SECONDS: %r" % s)
        sends.append((float(when), codecs.decode(text, "unicode_escape").encode("latin-1")))
    a.sends = sorted(sends, key=lambda x: x[0])
    return a


def main():
    a = parse()
    master, slave = pty.openpty()
    if a.rows or a.cols:
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", a.rows or 24, a.cols or 80, 0, 0))
    before = termios.tcgetattr(master)

    pid = os.fork()
    if pid == 0:
        os.close(master)
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        for fd in (0, 1, 2):
            os.dup2(slave, fd)
        if slave > 2:
            os.close(slave)
        try:
            os.execvp(a.cmd[0], a.cmd)
        except OSError as e:
            os.write(2, ("ptyrun: %s: %s\n" % (a.cmd[0], e.strerror)).encode())
            os._exit(127)
    os.close(slave)

    start = time.monotonic()
    out = sys.stdout.buffer
    sends = list(a.sends)
    status = None
    eof = False
    while True:
        now = time.monotonic() - start
        if now >= a.timeout:
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(pid, 0)
            sys.exit(124)
        while sends and sends[0][0] <= now:
            os.write(master, sends.pop(0)[1])
        if status is None:
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                status = st
        if eof and status is not None:
            break
        wait = a.timeout - now
        if sends:
            wait = min(wait, sends[0][0] - now)
        if eof:  # the terminal is closed but CMD (or something holding it) still runs
            wait = min(wait, 0.05)
            time.sleep(max(wait, 0))
            continue
        r, _, _ = select.select([master], [], [], max(wait, 0) if status is None else 0.2)
        if not r:
            if status is not None:  # CMD is gone and the terminal went quiet
                break
            continue
        try:
            data = os.read(master, 65536)
        except OSError:  # EIO: every slave fd is closed
            data = b""
        if not data:
            eof = True
            continue
        out.write(data)
        out.flush()

    after = termios.tcgetattr(master)
    code = os.waitstatus_to_exitcode(status)
    if code < 0:
        code = 128 - code
    if a.stty_check and before != after:
        sys.stderr.write("ptyrun: terminal settings changed\n  before %r\n  after  %r\n" % (before, after))
        sys.exit(99)
    sys.exit(code)


if __name__ == "__main__":
    main()
