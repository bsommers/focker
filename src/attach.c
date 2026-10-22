/* focker fg: put this terminal on a family's pty. The supervisor hands over
 * the pty master; this copies bytes both ways in raw mode until the user
 * detaches (Ctrl-P Ctrl-Q), another fg takes over, or the job ends. The
 * terminal's settings are restored on every way out but SIGKILL. */
#include "focker.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <termios.h>
#include <unistd.h>

#define KEY_P 0x10 /* Ctrl-P */
#define KEY_Q 0x11 /* Ctrl-Q */

static struct termios saved;
static bool raw_on;

static void restore_tty(void)
{
    if (raw_on)
        tcsetattr(0, TCSANOW, &saved);
    raw_on = false;
}

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static void copy_winsize(int master)
{
    struct winsize ws;
    if (master >= 0 && ioctl(0, TIOCGWINSZ, &ws) == 0)
        ioctl(master, TIOCSWINSZ, &ws);
}

/* the job ended: show what it printed last, then say how it ended */
static void flush_master(int master)
{
    char buf[65536];
    for (;;) {
        struct pollfd p = { .fd = master, .events = POLLIN };
        if (poll(&p, 1, 50) <= 0)
            return;
        ssize_t n = read(master, buf, sizeof buf);
        if (n <= 0 || write_all(1, buf, (size_t)n) < 0)
            return;
    }
}

int cmd_fg(const char *name)
{
    if (!isatty(0) || tcgetattr(0, &saved) < 0) {
        fprintf(stderr, "focker: fg needs a terminal\n");
        return FK_FAIL;
    }
    int master, conn;
    int rc = client_attach(name, &master, &conn);
    if (rc < 0) {
        fprintf(stderr, "focker: no running family '%s' (%s)\n", name, strerror(errno));
        return FK_FAIL;
    }
    if (rc > 0)
        return FK_FAIL;

    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGWINCH);
    sigaddset(&m, SIGTERM);
    sigaddset(&m, SIGINT);
    sigaddset(&m, SIGHUP);
    sigaddset(&m, SIGQUIT);
    sigprocmask(SIG_BLOCK, &m, NULL);
    int sfd = signalfd(-1, &m, SFD_CLOEXEC);
    signal(SIGPIPE, SIG_IGN);

    struct termios raw = saved;
    cfmakeraw(&raw);
    raw_on = tcsetattr(0, TCSANOW, &raw) == 0;
    atexit(restore_tty);
    copy_winsize(master);

    char line[256];
    size_t llen = 0;
    bool pending_p = false; /* saw Ctrl-P; Ctrl-Q next detaches */
    int code = -1, sig = 0;
    const char *msg = NULL;
    char text[512];
    while (code < 0) {
        struct pollfd p[4] = {
            { .fd = 0, .events = POLLIN },
            { .fd = master, .events = POLLIN },
            { .fd = conn, .events = POLLIN },
            { .fd = sfd, .events = POLLIN },
        };
        if (poll(p, 4, -1) < 0) {
            if (errno == EINTR)
                continue;
            snprintf(text, sizeof text, "focker: poll: %s", strerror(errno));
            msg = text, code = FK_FAIL;
            break;
        }
        if (p[3].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGWINCH)
                    copy_winsize(master);
                else
                    sig = (int)si.ssi_signo, code = 128 + sig;
            }
        }
        if (p[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(conn, line + llen, sizeof line - 1 - llen);
            if (n > 0) {
                llen += (size_t)n;
                line[llen] = '\0';
            }
            char *nl = strchr(line, '\n');
            if (nl) {
                *nl = '\0';
                int c;
                char how[200];
                if (strcmp(line, "taken-over") == 0) {
                    msg = "taken over by another terminal", code = 0;
                } else if (sscanf(line, "exit %d %199[^\n]", &c, how) == 2) {
                    if (master >= 0)
                        flush_master(master);
                    snprintf(text, sizeof text, "%s: %s", name, how);
                    msg = text, code = c;
                } else {
                    snprintf(text, sizeof text, "focker: unexpected message from the supervisor: %s", line);
                    msg = text, code = FK_FAIL;
                }
            } else if (n <= 0 || llen == sizeof line - 1) {
                msg = "focker: supervisor went away", code = FK_FAIL;
            }
        }
        if (code >= 0)
            break;
        if (master >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[65536];
            ssize_t n = read(master, buf, sizeof buf);
            if (n > 0) {
                if (write_all(1, buf, (size_t)n) < 0) {
                    msg = "focker: cannot write to this terminal", code = FK_FAIL;
                    break;
                }
            } else if (n == 0 || errno != EINTR) { /* EIO: the job let go; its status comes on conn */
                close(master);
                master = -1;
            }
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char in[4096], out[4097];
            ssize_t n = read(0, in, sizeof in);
            if (n <= 0) {
                msg = NULL, code = 0; /* our terminal went away: same as a detach */
                break;
            }
            size_t o = 0;
            for (ssize_t i = 0; i < n && code < 0; i++) {
                unsigned char b = (unsigned char)in[i];
                if (pending_p) {
                    pending_p = false;
                    if (b == KEY_Q) {
                        snprintf(text, sizeof text, "detached from %s", name);
                        msg = text, code = 0;
                        break;
                    }
                    out[o++] = KEY_P;
                }
                if (b == KEY_P)
                    pending_p = true;
                else
                    out[o++] = (char)b;
            }
            if (master >= 0 && o > 0 && write_all(master, out, o) < 0) {
                close(master);
                master = -1;
            }
        }
    }

    restore_tty();
    close(conn); /* the supervisor resumes logging the job */
    if (master >= 0)
        close(master);
    if (msg)
        fprintf(stderr, "\n%s\n", msg);
    if (sig) {
        signal(sig, SIG_DFL);
        sigprocmask(SIG_UNBLOCK, &m, NULL);
        raise(sig);
    }
    return code;
}
