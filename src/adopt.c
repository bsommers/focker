/* adopt-external, supervisor side: move a job from another terminal under this
 * family. Stop it, dump each root, kill the originals, restore each root as a
 * child of the supervisor (on a new pty when the job had a terminal), check,
 * start it again. Until the originals are killed, any failure leaves the job
 * running where it was; after that, the images stay for a retry. The criu
 * options and the order come from .planning/phases/08-criu-adoption/08-SPIKE.md. */
#include "focker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define FD_BASE    10   /* the restorer's fd for the job's k-th pipe: FD_BASE + k */
#define FD_HIGH    100  /* where the new pipes' ends wait, clear of FD_BASE + k */
#define SETTLE_MS  5000 /* for the job to stop, and for the killed originals to be gone */

static void note(struct sbuf *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(struct sbuf *out, const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    sb_printf(out, "adopt: %s\n", line);
    logmsg("adopt: %s", line);
}

static void signal_all(const struct criu_job *j, int sig)
{
    kill(-j->pgid, sig);
    for (int m = 0; m < j->nmembers; m++)
        kill(j->members[m], sig);
}

/* every member stopped (want 'T') or gone (want 0, zombies included) */
static bool settle(const struct criu_job *j, char want)
{
    for (long deadline = now_ms() + SETTLE_MS;; sleep_ms(10)) {
        int m = 0;
        while (m < j->nmembers && proc_state(j->members[m]) == want)
            m++;
        if (m == j->nmembers)
            return true;
        if (now_ms() >= deadline)
            return false;
    }
}

/* the job's terminal as terminal A has it set: always its size; its settings
 * only when the job is in the foreground there (else a shell's own settings) */
static void read_terminal(const struct criu_job *j, struct termios *tio, bool *have_tio, struct winsize *ws,
                          bool *have_ws)
{
    *have_tio = *have_ws = false;
    unsigned major = ((unsigned)j->tty_nr >> 8) & 0xfff;
    unsigned minor = ((unsigned)j->tty_nr & 0xff) | (((unsigned)j->tty_nr >> 12) & 0xfff00);
    for (int fd = 0; fd <= 2 && !*have_ws; fd++) {
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/fd/%d", j->roots[0], fd);
        int t = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (t < 0)
            continue;
        struct stat st;
        if (fstat(t, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == makedev(major, minor)) {
            *have_ws = ioctl(t, TIOCGWINSZ, ws) == 0;
            *have_tio = proc_tpgid(j->roots[0]) == j->pgid && tcgetattr(t, tio) == 0;
        }
        close(t);
    }
}

/* run criu with stdio on io (/dev/null if -1), from[i] at fd to[i], in
 * process group pgrp if > 0; its wait status, or -1. Killed after
 * CRIU_TIMEOUT_MS. */
static int run_criu(char **argv, int io, const int *from, const int *to, int nmove, pid_t pgrp)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        if (pgrp > 0)
            setpgid(0, pgrp); /* criu restores roots that do not lead their group into its own */
        int null = open("/dev/null", O_RDWR);
        int s = io >= 0 ? io : null;
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        for (int i = 0; i < nmove; i++)
            dup2(from[i], to[i]); /* from is >= FD_HIGH, so never one of the targets */
        execv(argv[0], argv);
        _exit(127);
    }
    long deadline = now_ms() + CRIU_TIMEOUT_MS;
    for (;;) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid)
            return st;
        if (r < 0 && errno != EINTR)
            return -1;
        if (now_ms() >= deadline)
            kill(pid, SIGKILL);
        sleep_ms(10);
    }
}

/* A new process group for the restored job. The shell in terminal A may still
 * list the old group as a job (bash does until it next reports, and sends
 * SIGHUP to every group it lists when its terminal closes), so the job must
 * not keep that group. The new group's leader is a child that waits on *hold
 * and leaves once the job has joined; the group lives on with the job. */
static pid_t new_group(int *hold)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[1]);
        setpgid(0, 0);
        char b;
        while (read(p[0], &b, 1) < 0 && errno == EINTR)
            ;
        _exit(0);
    }
    close(p[0]);
    if (pid < 0) {
        close(p[1]);
        return -1;
    }
    setpgid(pid, pid);
    *hold = p[1];
    return pid;
}

static void end_group(pid_t pid, int hold)
{
    if (pid <= 0)
        return;
    close(hold);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

/* make criu restore the group led by root into criu's own group: with
 * --shell-job, criu moves every task whose group's leader is not in the
 * images into the restorer's group, so the leader's pgid becomes grp, which
 * is not in the images either */
static int regroup(const char *dir, pid_t root, pid_t grp, char *why, size_t len)
{
    char path[PATH_MAX + 64], tmp[PATH_MAX + 64];
    snprintf(path, sizeof path, "%.*s/pstree.img", PATH_MAX, dir);
    snprintf(tmp, sizeof tmp, "%.*s/pstree.img.new", PATH_MAX, dir);
    int rc = -1;
    unsigned char *in = NULL, *out = NULL;
    struct stat st = { 0 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0 && fstat(fd, &st) == 0 && (in = malloc((size_t)st.st_size + 1)) &&
        (out = malloc((size_t)st.st_size * 2 + 64)) && read(fd, in, (size_t)st.st_size) == st.st_size) {
        int n = criu_pstree_regroup(in, (size_t)st.st_size, (unsigned long)root, (unsigned long)grp, out,
                                    (size_t)st.st_size * 2 + 64);
        int w = n < 0 ? -1 : open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (w >= 0) {
            rc = write(w, out, (size_t)n) == n && close(w) == 0 && rename(tmp, path) == 0 ? 0 : -1;
            if (rc < 0)
                unlink(tmp);
        }
    }
    if (rc < 0)
        snprintf(why, len, "cannot rewrite %s for the new process group", path);
    if (fd >= 0)
        close(fd);
    free(in);
    free(out);
    return rc;
}

static bool exited_ok(int st)
{
    return st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void argv_text(char **argv, char *buf, size_t len)
{
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; argv[i] && used < len; i++)
        used += (size_t)snprintf(buf + used, len - used, "%s%s", i ? " " : "", argv[i]);
}

/* put the buffered bytes of pipe ino back into the new pipe (criu does not) */
static int refill(const char *imgs, const struct criu_job *j, unsigned long ino, int wfd, char *why, size_t len)
{
    for (int r = 0; r < j->nroots; r++) {
        char path[PATH_MAX + 64];
        snprintf(path, sizeof path, "%s/%d/pipes-data.img", imgs, j->roots[r]);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        struct stat st;
        unsigned char *img = NULL;
        ssize_t got = -1;
        if (fstat(fd, &st) == 0 && (img = malloc((size_t)st.st_size + 1)))
            got = read(fd, img, (size_t)st.st_size);
        close(fd);
        size_t off, n;
        unsigned long size;
        int found = got == st.st_size ? criu_pipe_data(img, (size_t)got, ino, &off, &n, &size) : -1;
        int rc = 0;
        if (found < 0) {
            snprintf(why, len, "%s: cannot read it", path);
            rc = -1;
        } else if (found == 1 && n > 0) {
            if (size > 0 && fcntl(wfd, F_SETPIPE_SZ, (int)size) < 0 && (size_t)fcntl(wfd, F_GETPIPE_SZ) < n) {
                snprintf(why, len, "cannot size pipe:[%lu] to %lu: %s", ino, size, strerror(errno));
                rc = -1;
            }
            fcntl(wfd, F_SETFL, O_NONBLOCK); /* a pipe smaller than its data must not hang the supervisor */
            for (size_t done = 0; rc == 0 && done < n;) {
                ssize_t w = write(wfd, img + off + done, n - done);
                if (w < 0 && errno == EINTR)
                    continue;
                if (w <= 0) {
                    snprintf(why, len, "cannot refill pipe:[%lu]: %s", ino, strerror(errno));
                    rc = -1;
                }
                done += w > 0 ? (size_t)w : 0;
            }
            fcntl(wfd, F_SETFL, 0);
        }
        free(img);
        if (rc < 0 || found == 1)
            return rc;
    }
    return 0;
}

int adopt_run(struct family *f, const struct criu_job *j, const char *criu, int *master, struct sbuf *out)
{
    *master = -1;
    char imgs[PATH_MAX + 8], why[PATH_MAX + 2048], sub[MAX_ROOTS][PATH_MAX + 32];
    snprintf(imgs, sizeof imgs, "%s/criu", f->dir);
    for (int r = 0; r < j->nroots; r++)
        snprintf(sub[r], sizeof sub[r], "%s/%d", imgs, j->roots[r]);
    pid_t old_ppid[MAX_PIDS];
    for (int m = 0; m < j->nmembers; m++)
        old_ppid[m] = proc_ppid(j->members[m]);

    /* before the stop: a stopped job hands terminal A back to its shell */
    struct termios tio;
    struct winsize ws;
    bool have_tio = false, have_ws = false;
    if (j->tty)
        read_terminal(j, &tio, &have_tio, &ws, &have_ws);
    /* the stop hands a foreground job's terminal back to its shell, so a resumed job runs in the background */
    const char *where = j->tty && proc_tpgid(j->roots[0]) == j->pgid
                            ? "continues, in the background in its terminal; 'fg' there brings it back"
                            : "continues where it was";

    signal_all(j, SIGSTOP);
    if (!settle(j, 'T')) {
        signal_all(j, SIGCONT);
        note(out, "could not stop job %d (did it end?); it %s", j->pgid, where);
        return 1;
    }
    note(out, "stopped job %d (%d process%s)", j->pgid, j->nmembers, j->nmembers == 1 ? "" : "es");

    /* 1. dump each root, leaving it stopped */
    int rc = 0;
    if (mkdir(imgs, 0700) < 0 && errno != EEXIST) {
        snprintf(why, sizeof why, "%s: %s", imgs, strerror(errno));
        rc = -1;
    }
    for (int r = 0; r < j->nroots && rc == 0; r++) {
        rm_tree(sub[r]);
        if (mkdir(sub[r], 0700) < 0) {
            snprintf(why, sizeof why, "%.*s: %s", (int)sizeof sub[r], sub[r], strerror(errno));
            rc = -1;
            break;
        }
        char *argv[32], log[PATH_MAX + 64];
        snprintf(log, sizeof log, "%.*s/dump.log", (int)sizeof sub[r], sub[r]);
        if (criu_dump_argv(criu, j, j->roots[r], sub[r], false, argv, 32) < 0) {
            snprintf(why, sizeof why, "criu path too long");
            rc = -1;
        } else if (!exited_ok(run_criu(argv, -1, NULL, NULL, 0, 0))) {
            criu_log_error(log, j->roots[r], why, sizeof why);
            rc = -1;
        } else {
            note(out, "dumped %d", j->roots[r]);
        }
    }
    int slave = -1;
    if (rc == 0 && j->tty &&
        (pty_open(master, &slave, have_tio ? &tio : NULL, have_ws ? &ws : NULL) < 0 || pty_take_ctty(slave) < 0)) {
        snprintf(why, sizeof why, "cannot set up a terminal: %s", strerror(errno));
        if (slave >= 0) {
            close(*master);
            close(slave);
            *master = slave = -1;
        }
        rc = -1;
    }
    /* the new group, made before the kill so its leader cannot take a PID the job needs */
    int hold = -1;
    pid_t grp = 0;
    if (rc == 0 && (grp = new_group(&hold)) < 0) {
        snprintf(why, sizeof why, "cannot start a process group: %s", strerror(errno));
        grp = 0;
        rc = -1;
    }
    for (int r = 0; r < j->nroots && rc == 0; r++)
        if (j->roots[r] == j->pgid)
            rc = regroup(sub[r], j->pgid, grp, why, sizeof why);
    if (rc < 0) {
        end_group(grp, hold);
        if (slave >= 0) {
            close(*master);
            close(slave);
            *master = slave = -1;
        }
        signal_all(j, SIGCONT);
        for (int r = 0; r < j->nroots; r++)
            rm_tree(sub[r]);
        note(out, "dump failed: %s; job %d %s", why, j->pgid, where);
        return 1;
    }

    /* 2. the point of no return */
    note(out, "killed the originals (point of no return)");
    signal_all(j, SIGKILL);
    int pr[MAX_ROOTS], pw[MAX_ROOTS], np = 0;
    char *argv[16 + MAX_ROOTS * 2];
    int failed = -1; /* the root whose restore failed */
    if (!settle(j, 0)) {
        snprintf(why, sizeof why, "the originals are still there after %d ms (their parent has not reaped them)",
                 SETTLE_MS);
        failed = 0;
    }

    /* 3. one new pipe per pipe between roots, with the bytes it held */
    for (; failed < 0 && np < j->npipes; np++) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) < 0) {
            snprintf(why, sizeof why, "pipe: %s", strerror(errno));
            failed = 0;
            break;
        }
        pr[np] = fcntl(p[0], F_DUPFD_CLOEXEC, FD_HIGH);
        pw[np] = fcntl(p[1], F_DUPFD_CLOEXEC, FD_HIGH);
        close(p[0]);
        close(p[1]);
        if (pr[np] < 0 || pw[np] < 0 || refill(imgs, j, j->pipes[np], pw[np], why, sizeof why) < 0) {
            if (pr[np] < 0 || pw[np] < 0)
                snprintf(why, sizeof why, "fcntl: %s", strerror(errno));
            np++;
            failed = 0;
            break;
        }
    }

    /* 4. restore each root into the new group (criu runs in it), the old group's leader first */
    int order[MAX_ROOTS] = { 0 }, no = 0;
    for (int r = 0; r < j->nroots; r++)
        if (j->roots[r] == j->pgid)
            order[no++] = r;
    for (int r = 0; r < j->nroots; r++)
        if (j->roots[r] != j->pgid)
            order[no++] = r;
    int restored = 0;
    for (; failed < 0 && restored < j->nroots; restored++) {
        int r = order[restored];
        int pipe_fds[MAX_ROOTS], from[MAX_ROOTS], to[MAX_ROOTS], nmove = 0;
        for (int k = 0; k < j->npipes; k++) {
            bool rd = j->pipe_rd[k] >> r & 1, wr = j->pipe_wr[k] >> r & 1;
            pipe_fds[k] = rd || wr ? FD_BASE + k : -1;
            if (rd || wr) {
                from[nmove] = rd ? pr[k] : pw[k];
                to[nmove++] = FD_BASE + k;
            }
        }
        char pidfile[PATH_MAX + 64], log[PATH_MAX + 64];
        snprintf(pidfile, sizeof pidfile, "%s/restored.pid", sub[r]);
        snprintf(log, sizeof log, "%s/restore.log", sub[r]);
        unlink(pidfile);
        if (criu_restore_argv(criu, j, sub[r], pipe_fds, argv, 16 + MAX_ROOTS * 2) < 0) {
            snprintf(why, sizeof why, "criu path too long");
            failed = r;
        } else if (!exited_ok(run_criu(argv, slave, from, to, nmove, grp))) {
            criu_log_error(log, j->roots[r], why, sizeof why);
            failed = r;
        } else if (proc_pgid(j->roots[r]) != grp && setpgid(j->roots[r], grp) < 0) {
            snprintf(why, sizeof why, "setpgid %d: %s", j->roots[r], strerror(errno));
            failed = r;
        } else {
            note(out, "restored %d", j->roots[r]);
        }
    }
    for (int k = 0; k < np; k++) {
        close(pr[k]);
        close(pw[k]);
    }

    /* 5. check: every process back, the roots the supervisor's children */
    for (int m = 0; failed < 0 && m < j->nmembers; m++) {
        pid_t p = j->members[m];
        char st = proc_state(p);
        bool root = false;
        for (int r = 0; r < j->nroots; r++)
            root |= j->roots[r] == p;
        if (st == 0 || st == 'Z' || !proc_is_descendant(p, getpid()) || (root && proc_ppid(p) != getpid())) {
            snprintf(why, sizeof why, "%d did not come back under the supervisor", p);
            failed = 0;
        }
    }
    if (failed >= 0) {
        for (int m = 0; m < j->nmembers; m++)
            if (proc_is_descendant(j->members[m], getpid()))
                kill(j->members[m], SIGKILL);
        end_group(grp, hold);
        if (slave >= 0) {
            close(slave);
            close(*master);
            *master = -1;
        }
        note(out, "restore failed: %s", why);
        for (int r = 0; r < j->nroots; r++) {
            int pipe_fds[MAX_ROOTS];
            for (int k = 0; k < j->npipes; k++)
                pipe_fds[k] = (j->pipe_rd[k] | j->pipe_wr[k]) >> r & 1 ? FD_BASE + k : -1;
            char text[4096] = "?";
            if (criu_restore_argv(criu, j, sub[r], pipe_fds, argv, 16 + MAX_ROOTS * 2) >= 0)
                argv_text(argv, text, sizeof text);
            note(out, "restore failed; images kept in %s; retry with: %s", sub[r], text);
        }
        return 1;
    }

    for (int m = 0; m < j->nmembers; m++) {
        struct member *mb = family_member(f, j->members[m], true);
        if (!mb)
            continue;
        mb->origin = 'x';
        mb->old_ppid = old_ppid[m];
        proc_cmdline(j->members[m], mb->cmd, sizeof mb->cmd);
    }
    if (slave >= 0) {
        void (*old)(int) = signal(SIGTTOU, SIG_IGN);
        tcsetpgrp(slave, grp);
        signal(SIGTTOU, old);
        close(slave);
    }
    for (int m = 0; m < j->nmembers; m++)
        if (proc_pgid(j->members[m]) != grp)
            note(out, "warning: %d stays in process group %d, which the shell in the old terminal may still signal",
                 j->members[m], proc_pgid(j->members[m]));
    kill(-grp, SIGCONT);
    signal_all(j, SIGCONT);
    end_group(grp, hold);
    for (int r = 0; r < j->nroots; r++)
        rm_tree(sub[r]);
    rmdir(imgs);
    note(out, "job %d runs under %s as process group %d%s", j->pgid, f->name, grp,
         j->tty ? " on a new terminal ('focker fg' it)" : "");
    return 0;
}
