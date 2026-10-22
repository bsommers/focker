/* adopt-external, client side: everything that can be checked before a job in
 * another terminal is touched. Find a capable criu, work out the job (the
 * process group of PID, so a whole pipeline), refuse what cannot move cleanly,
 * trial-dump each root while the job keeps running, then ask. criu options are
 * the ones .planning/phases/08-criu-adoption/08-SPIKE.md found to work. */
#include "focker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- job discovery --------------------------------------------------------- */

static int cmp_pid(const void *a, const void *b)
{
    pid_t x = *(const pid_t *)a, y = *(const pid_t *)b;
    return (x > y) - (x < y);
}

static int index_of(const struct procent *ps, int n, pid_t pid)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ps[mid].pid == pid)
            return mid;
        if (ps[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int job_roots(const struct procent *ps, int n, const pid_t *pgids, pid_t pgid, pid_t *roots, int max)
{
    int nr = 0;
    for (int i = 0; i < n; i++) {
        if (pgids[i] != pgid)
            continue;
        int p = index_of(ps, n, ps[i].ppid);
        if (p >= 0 && pgids[p] == pgid)
            continue;
        if (nr == max) {
            errno = E2BIG;
            return -1;
        }
        roots[nr++] = ps[i].pid;
    }
    qsort(roots, (size_t)nr, sizeof roots[0], cmp_pid); /* ps is sorted, but keep the promise explicit */
    return nr;
}

static bool in_list(const pid_t *list, int n, pid_t pid)
{
    for (int i = 0; i < n; i++)
        if (list[i] == pid)
            return true;
    return false;
}

static void say(char *why, size_t len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *why, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, len, fmt, ap);
    va_end(ap);
}

/* pipes the job's processes hold: which roots' subtrees hold each one */
struct pipe_use { unsigned long ino; unsigned roots, rd, wr; pid_t holder; };

static int collect_pipes(const struct criu_job *j, const pid_t *root_of, struct pipe_use *pu, int max)
{
    int np = 0;
    static struct fdent fds[MAX_FDENTS];
    for (int m = 0; m < j->nmembers; m++) {
        int nf = proc_fds(j->members[m], fds, MAX_FDENTS);
        int r = 0;
        while (r < j->nroots && j->roots[r] != root_of[m])
            r++;
        for (int f = 0; f < nf; f++) {
            if (fds[f].kind != 'p')
                continue;
            int k = 0;
            while (k < np && pu[k].ino != fds[f].ino)
                k++;
            if (k == np) {
                if (np == max)
                    return -1;
                pu[np++] = (struct pipe_use){ .ino = fds[f].ino, .holder = j->members[m] };
            }
            pu[k].roots |= 1u << r;
            if (fds[f].accmode == O_RDONLY)
                pu[k].rd |= 1u << r;
            else
                pu[k].wr |= 1u << r;
        }
    }
    return np;
}

int criu_job_load(pid_t pid, struct criu_job *j, char *why, size_t len)
{
    memset(j, 0, sizeof *j);
    j->pgid = proc_pgid(pid);
    if (j->pgid <= 0) {
        say(why, len, "no process %d", pid);
        return -1;
    }
    /* the job must not be the one running focker: moving it would kill this command */
    for (pid_t a = getpid(); a > 1; a = proc_ppid(a))
        if (proc_pgid(a) == j->pgid) {
            say(why, len, "%d's job includes focker itself (or the shell running it)", pid);
            return -1;
        }

    struct procent *ps;
    int n = proc_list(&ps);
    if (n < 0) {
        say(why, len, "cannot list processes: %s", strerror(errno));
        return -1;
    }
    pid_t *pgids = malloc(((size_t)n + 1) * sizeof *pgids);
    int rc = -1;
    if (!pgids) {
        say(why, len, "out of memory");
        goto out;
    }
    for (int i = 0; i < n; i++)
        pgids[i] = proc_pgid(ps[i].pid);
    j->nroots = job_roots(ps, n, pgids, j->pgid, j->roots, MAX_ROOTS);
    if (j->nroots < 0) {
        say(why, len, "%d's job has more than %d separate processes", pid, MAX_ROOTS);
        goto out;
    }
    /* members: every root's whole subtree, in tree order (a parent before its children) */
    pid_t mroot[MAX_PIDS];
    for (int r = 0; r < j->nroots; r++) {
        j->members[j->nmembers] = j->roots[r];
        mroot[j->nmembers++] = j->roots[r];
    }
    for (int m = 0; m < j->nmembers; m++)
        for (int i = 0; i < n; i++) {
            if (ps[i].ppid != j->members[m])
                continue;
            if (j->nmembers == MAX_PIDS) {
                say(why, len, "%d's job has more than %d processes", pid, MAX_PIDS);
                goto out;
            }
            j->members[j->nmembers] = ps[i].pid;
            mroot[j->nmembers++] = mroot[m];
        }
    for (int m = 0; m < j->nmembers; m++) {
        pid_t p = j->members[m];
        if (proc_uid(p) != getuid()) {
            say(why, len, "%d (in %d's job) belongs to another user", p, pid);
            goto out;
        }
        if (proc_sid(p) == p) {
            say(why, len, "%d leads its own session (a shell?); adopt a job it started instead", p);
            goto out;
        }
    }
    j->tty_nr = proc_tty_nr(j->roots[0]);
    j->tty = j->tty_nr > 0;

    struct pipe_use pu[MAX_FDENTS];
    int np = collect_pipes(j, mroot, pu, MAX_FDENTS);
    if (np < 0) {
        say(why, len, "%d's job holds too many pipes", pid);
        goto out;
    }
    for (int k = 0; k < np; k++) {
        if ((pu[k].roots & (pu[k].roots - 1)) == 0)
            continue; /* one root's own pipe: it moves inside that dump */
        if (j->npipes == MAX_ROOTS) {
            say(why, len, "%d's job has more than %d pipes between its processes", pid, MAX_ROOTS);
            goto out;
        }
        if (pu[k].rd & pu[k].wr) { /* each root's restore gets one end of the new pipe, not both */
            say(why, len, "%d's pipe:[%lu] is held at both ends by one part of the job and read or written by another",
                pu[k].holder, pu[k].ino);
            goto out;
        }
        j->pipe_rd[j->npipes] = pu[k].rd;
        j->pipe_wr[j->npipes] = pu[k].wr;
        j->pipes[j->npipes++] = pu[k].ino;
    }
    /* a pipe that leads out of the job would be cut by the move: criu does not refuse it, so focker does */
    static struct fdent fds[MAX_FDENTS];
    for (int i = 0; i < n; i++) {
        if (in_list(j->members, j->nmembers, ps[i].pid) || proc_uid(ps[i].pid) != getuid())
            continue;
        int nf = proc_fds(ps[i].pid, fds, MAX_FDENTS);
        for (int f = 0; f < nf; f++)
            for (int k = 0; fds[f].kind == 'p' && k < np; k++)
                if (pu[k].ino == fds[f].ino) {
                    say(why, len, "%d's pipe:[%lu] leads to %d outside the job (moving it would cut that pipe)",
                        pu[k].holder, pu[k].ino, ps[i].pid);
                    goto out;
                }
    }
    rc = 0;
out:
    free(pgids);
    free(ps);
    return rc;
}

/* ---- criu argv ------------------------------------------------------------- */

struct arena { char buf[8192]; size_t used; };

static char *put(struct arena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static char *put(struct arena *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t room = sizeof a->buf - a->used;
    int n = vsnprintf(a->buf + a->used, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room)
        return NULL;
    char *s = a->buf + a->used;
    a->used += (size_t)n + 1;
    return s;
}

/* argv[] from NULL-terminated pieces, or -1 if argv or the arena is too small */
static int fill(char **argv, int max, char **pieces, int n)
{
    if (n + 1 > max)
        return -1;
    for (int i = 0; i < n; i++)
        if (!(argv[i] = pieces[i]))
            return -1;
    argv[n] = NULL;
    return n;
}

int criu_dump_argv(const char *criu, const struct criu_job *j, pid_t root, const char *dir, bool trial, char **argv,
                   int max)
{
    (void)j; /* the terminal and the pipes between roots need no dump options (spike 2, 5) */
    static struct arena a;
    a.used = 0;
    char *p[] = { put(&a, "%s", criu), "dump", "--unprivileged", "--shell-job", "-t", put(&a, "%d", root), "-D",
                  put(&a, "%s", dir), "-o", "dump.log", trial ? "--leave-running" : "--leave-stopped" };
    return fill(argv, max, p, (int)(sizeof p / sizeof p[0]));
}

int criu_restore_argv(const char *criu, const struct criu_job *j, const char *dir, const int *pipe_fds, char **argv,
                      int max)
{
    static struct arena a;
    a.used = 0;
    char *p[16 + MAX_ROOTS * 2] = { put(&a, "%s", criu), "restore", "--unprivileged", "--shell-job",
                                    "--restore-detached", "-D", put(&a, "%s", dir), "-o", "restore.log",
                                    "--pidfile", put(&a, "%s/restored.pid", dir), "--leave-stopped" };
    int n = 12;
    for (int k = 0; k < j->npipes; k++)
        if (pipe_fds[k] >= 0) {
            p[n++] = "--inherit-fd";
            p[n++] = put(&a, "fd[%d]:pipe:[%lu]", pipe_fds[k], j->pipes[k]);
        }
    return fill(argv, max, p, n);
}

/* ---- running criu ---------------------------------------------------------- */

int criu_find(char *path, size_t len)
{
    const char *env = getenv("FOCKER_CRIU");
    if (env && *env) {
        snprintf(path, len, "%s", env);
        return access(path, X_OK) == 0 ? 0 : -1;
    }
    const char *dirs = getenv("PATH");
    char buf[4096];
    snprintf(buf, sizeof buf, "%s:/usr/sbin:/usr/local/sbin:/sbin", dirs ? dirs : "/usr/bin:/bin");
    for (char *save, *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(path, len, "%s/criu", d);
        if (access(path, X_OK) == 0)
            return 0;
    }
    return -1;
}

/* run argv with stdin from /dev/null and stdout+stderr into out (if given),
 * killing it after timeout_ms. Its wait status, or -1. */
static int run_capture(char **argv, char *out, size_t outlen, int timeout_ms)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return -1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0)
            dup2(null, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    size_t got = 0;
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        long left = deadline - now_ms();
        struct pollfd pf = { .fd = p[0], .events = POLLIN };
        int r = left > 0 ? poll(&pf, 1, (int)left) : 0;
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) { /* timed out */
            kill(pid, SIGKILL);
            break;
        }
        char buf[4096];
        ssize_t n = read(p[0], buf, sizeof buf);
        if (n <= 0)
            break;
        if (out && got + 1 < outlen) {
            size_t take = (size_t)n < outlen - 1 - got ? (size_t)n : outlen - 1 - got;
            memcpy(out + got, buf, take);
            got += take;
        }
    }
    if (out && outlen)
        out[got] = '\0';
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    return st;
}

int criu_capable(const char *criu, char *why, size_t len)
{
    char out[4096];
    char *argv[] = { (char *)criu, "check", "--unprivileged", NULL };
    int st = run_capture(argv, out, sizeof out, CRIU_TIMEOUT_MS);
    /* only the capability counts here: 'check' also fails on host limits a dump
     * never meets (seen: no inotify instances left), and the trial dump tests the rest */
    if (st >= 0 && WIFEXITED(st) && WEXITSTATUS(st) != 127 && !strstr(out, "CAP_CHECKPOINT_RESTORE"))
        return 1;
    char *line = strstr(out, "CAP_CHECKPOINT_RESTORE") ? "lacks CAP_CHECKPOINT_RESTORE" : out;
    char *nl = strchr(line, '\n');
    say(why, len, "%.*s", nl ? (int)(nl - line) : (int)strlen(line), line[0] ? line : "criu check failed");
    return 0;
}

/* ---- the trial dump -------------------------------------------------------- */

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

void rm_tree(const char *dir)
{
    nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* why criu failed, from its log: the ptrace refusal first (it is only a
 * warning), else the first error without its "Error (file:line): " prefix */
void criu_log_error(const char *log, pid_t root, char *why, size_t len)
{
    char text[65536] = "";
    FILE *f = fopen(log, "re");
    if (f) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = '\0';
        fclose(f);
    }
    if (strstr(text, "Unable to interrupt task") && strstr(text, "Operation not permitted")) {
        char scope[16] = "";
        FILE *y = fopen("/proc/sys/kernel/yama/ptrace_scope", "re");
        if (y) {
            if (!fgets(scope, sizeof scope, y))
                scope[0] = '\0';
            fclose(y);
        }
        scope[strcspn(scope, "\n")] = '\0';
        if (scope[0] && strcmp(scope, "0") != 0) {
            say(why, len,
                "criu may not attach to %d: Yama ptrace_scope is %s, so only a process's own ancestors may.\n"
                "Allow it with:\n  sudo sysctl kernel.yama.ptrace_scope=0\n"
                "(and 'sudo sysctl kernel.yama.ptrace_scope=%s' to set it back)",
                root, scope, scope);
            return;
        }
    }
    for (char *line = text; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char *e = strstr(line, "Error (");
        char *msg = e ? strstr(e, "): ") : NULL;
        if (msg) {
            say(why, len, "%s", msg + 3);
            return;
        }
        line = nl ? nl + 1 : NULL;
    }
    say(why, len, "criu failed (no error in its log)");
}

static int trial_dump(const char *criu, const struct criu_job *j, char *why, size_t len)
{
    char rt[PATH_MAX], dir[PATH_MAX + 32];
    if (runtime_dir(rt, sizeof rt) < 0) {
        say(why, len, "cannot use runtime dir: %s", strerror(errno));
        return -1;
    }
    snprintf(dir, sizeof dir, "%s/.adopt-XXXXXX", rt);
    if (!mkdtemp(dir)) {
        say(why, len, "%s: %s", dir, strerror(errno));
        return -1;
    }
    int rc = 0;
    for (int r = 0; r < j->nroots && rc == 0; r++) {
        char sub[PATH_MAX + 64], log[PATH_MAX + 80];
        snprintf(sub, sizeof sub, "%s/%d", dir, j->roots[r]);
        snprintf(log, sizeof log, "%s/dump.log", sub);
        if (mkdir(sub, 0700) < 0) {
            say(why, len, "%s: %s", sub, strerror(errno));
            rc = -1;
            break;
        }
        char *argv[32];
        if (criu_dump_argv(criu, j, j->roots[r], sub, true, argv, 32) < 0) {
            say(why, len, "criu path too long");
            rc = -1;
            break;
        }
        int st = run_capture(argv, NULL, 0, CRIU_TIMEOUT_MS);
        if (st < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            criu_log_error(log, j->roots[r], why, len);
            rc = -1;
        }
    }
    rm_tree(dir);
    return rc;
}

/* ---- the command ----------------------------------------------------------- */

/* the family that already holds pid (as its supervisor or a member), or NULL */
static const char *family_of(pid_t pid, char *name, size_t len)
{
    char rt[PATH_MAX];
    if (runtime_dir(rt, sizeof rt) < 0)
        return NULL;
    DIR *d = opendir(rt);
    if (!d)
        return NULL;
    const char *found = NULL;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (!valid_name(e->d_name))
            continue;
        pid_t sup;
        if (client_info(e->d_name, &sup, NULL, NULL, 0) != 0)
            continue;
        bool hit = sup == pid;
        char *args[] = { "members" };
        struct sbuf r = { 0 };
        if (!hit && client_request(e->d_name, args, 1, &r) == 0)
            for (char *line = r.p; line && *line && !hit;) {
                int mp, old, alive;
                char origin;
                hit = sscanf(line, "%d %c %d %d", &mp, &origin, &old, &alive) == 4 && mp == pid && alive;
                line = strchr(line, '\n');
                line = line ? line + 1 : NULL;
            }
        sb_free(&r);
        if (hit) {
            snprintf(name, len, "%.*s", NAME_MAX_LEN, e->d_name); /* valid_name: at most 48 */
            found = name;
        }
    }
    closedir(d);
    return found;
}

static void tty_name(int tty_nr, char *buf, size_t len)
{
    unsigned major = ((unsigned)tty_nr >> 8) & 0xfff;
    unsigned minor = ((unsigned)tty_nr & 0xff) | (((unsigned)tty_nr >> 12) & 0xfff00);
    if (major >= 136 && major <= 143)
        snprintf(buf, len, "/dev/pts/%u", minor + (major - 136) * 256);
    else
        snprintf(buf, len, "terminal %u:%u", major, minor);
}

int cmd_adopt_external(const char *name, pid_t pid, bool yes, bool dry_run)
{
    char criu[PATH_MAX], why[2048], fam[NAME_MAX_LEN + 1];
    if (criu_find(criu, sizeof criu) < 0) {
        fprintf(stderr, "focker: criu not found (install it, or set FOCKER_CRIU)\n");
        return FK_FAIL;
    }
    if (!criu_capable(criu, why, sizeof why)) {
        if (strstr(why, "CAP_CHECKPOINT_RESTORE"))
            fprintf(stderr, "focker: criu at %s lacks CAP_CHECKPOINT_RESTORE; grant it once with:\n"
                            "  sudo setcap cap_checkpoint_restore+eip %s\n", criu, criu);
        else
            fprintf(stderr, "focker: 'criu check --unprivileged' failed: %s\n", why);
        return FK_FAIL;
    }
    if (proc_uid(pid) == (uid_t)-1) {
        fprintf(stderr, "focker: no process %d\n", pid);
        return FK_FAIL;
    }
    if (proc_uid(pid) != getuid()) {
        fprintf(stderr, "focker: %d belongs to another user\n", pid);
        return FK_FAIL;
    }
    if (family_of(pid, fam, sizeof fam)) {
        fprintf(stderr, "focker: %d is already in family %s\n", pid, fam);
        return FK_FAIL;
    }
    struct criu_job j;
    if (criu_job_load(pid, &j, why, sizeof why) < 0) {
        fprintf(stderr, "focker: %s; nothing was changed\n", why);
        return FK_FAIL;
    }
    for (int m = 0; m < j.nmembers; m++)
        if (family_of(j.members[m], fam, sizeof fam)) {
            fprintf(stderr, "focker: %d (in %d's job) is already in family %s\n", j.members[m], pid, fam);
            return FK_FAIL;
        }

    char dir[PATH_MAX];
    if (family_dir(name, dir, sizeof dir) < 0) {
        fprintf(stderr, "focker: cannot use runtime dir: %s\n", strerror(errno));
        return FK_FAIL;
    }
    bool existing = client_info(name, NULL, NULL, NULL, 0) == 0;
    if (!existing && access(dir, F_OK) == 0) {
        fprintf(stderr, "focker: name '%s' belongs to an exited family; 'focker rm %s' first\n", name, name);
        return FK_FAIL;
    }

    if (trial_dump(criu, &j, why, sizeof why) < 0) {
        fprintf(stderr, "focker: criu cannot move %d: %s\nnothing was changed\n", pid, why);
        return FK_FAIL;
    }

    char tty[64];
    tty_name(j.tty_nr, tty, sizeof tty);
    printf("plan: job %d: roots", j.pgid);
    for (int r = 0; r < j.nroots; r++)
        printf(" %d", j.roots[r]);
    printf("\nplan: processes");
    for (int m = 0; m < j.nmembers; m++)
        printf(" %d", j.members[m]);
    printf("\nplan: %s%s\n", j.tty ? "terminal " : "no terminal", j.tty ? tty : "");
    printf("plan: family %s (%s)\nplan: images in %s/criu\n", name, existing ? "existing" : "new", dir);
    fflush(stdout);
    if (dry_run)
        return FK_OK;

    if (!yes) {
        if (!isatty(0)) {
            fprintf(stderr, "focker: no terminal to confirm on; pass --yes to move the job without asking\n");
            return FK_FAIL;
        }
        fprintf(stderr, "kill the original and restore under %s? [y/N] ", name);
        char answer[32] = "";
        if (!fgets(answer, sizeof answer, stdin))
            answer[0] = '\0';
        answer[strcspn(answer, "\r\n")] = '\0';
        if (strcasecmp(answer, "y") != 0 && strcasecmp(answer, "yes") != 0) {
            fprintf(stderr, "focker: not moved; nothing was changed\n");
            return FK_FAIL;
        }
    }

    /* the move happens in the family's supervisor, so the job ends up its child */
    bool made = false;
    if (!existing) {
        if (mkdir(dir, 0700) < 0) {
            fprintf(stderr, "focker: %s: %s\n", dir, strerror(errno));
            return FK_FAIL;
        }
        if (start_detached(name, dir, NULL, true, false) != FK_OK)
            return FK_FAIL;
        made = true;
    }
    static char tok[4 + MAX_ROOTS][PATH_MAX + 16];
    char *args[4 + MAX_ROOTS];
    int n = 0;
    args[n++] = "adopt";
    snprintf(tok[n], sizeof tok[n], "criu=%s", criu), args[n] = tok[n], n++;
    snprintf(tok[n], sizeof tok[n], "pgid=%d", j.pgid), args[n] = tok[n], n++;
    if (made)
        args[n++] = "new=1";
    for (int r = 0; r < j.nroots; r++)
        snprintf(tok[n], sizeof tok[n], "root=%d", j.roots[r]), args[n] = tok[n], n++;
    struct sbuf reply = { 0 };
    int rc = client_request(name, args, n, &reply);
    if (rc < 0)
        fprintf(stderr, "focker: lost family '%s' during the move (%s); see 'focker logs %s'\n", name,
                strerror(errno), name);
    else
        fprintf(rc == 0 ? stdout : stderr, "%.*s", (int)reply.len, reply.p ? reply.p : "");
    bool touched = reply.p && strstr(reply.p, "point of no return");
    sb_free(&reply);
    if (rc != 0 && made && !touched) { /* nothing moved: the family made for the job goes too */
        char *stop[] = { "stop" };
        struct sbuf r = { 0 };
        client_request(name, stop, 1, &r);
        sb_free(&r);
        char sock[PATH_MAX + 16];
        snprintf(sock, sizeof sock, "%s/control.sock", dir);
        for (int i = 0; i < 100 && access(sock, F_OK) == 0; i++)
            sleep_ms(30);
        cmd_rm(name, true);
    }
    return rc == 0 ? FK_OK : FK_FAIL;
}

/* ---- pipes-data.img -------------------------------------------------------- */

static bool varint(const unsigned char **p, const unsigned char *end, unsigned long *v)
{
    *v = 0;
    for (int shift = 0; *p < end && shift < 64; shift += 7) {
        unsigned char b = *(*p)++;
        *v |= (unsigned long)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return true;
    }
    return false;
}

int criu_pipe_data(const unsigned char *img, size_t len, unsigned long ino, size_t *off, size_t *n,
                   unsigned long *size)
{
    size_t at = 8; /* the image's magic */
    while (at < len) {
        if (len - at < 4)
            return -1;
        size_t elen = (size_t)img[at] | (size_t)img[at + 1] << 8 | (size_t)img[at + 2] << 16 |
                      (size_t)img[at + 3] << 24;
        at += 4;
        if (elen > len - at)
            return -1;
        const unsigned char *p = img + at, *end = p + elen;
        unsigned long id = 0, bytes = 0, sz = 0;
        while (p < end) {
            unsigned long tag, v;
            if (!varint(&p, end, &tag))
                return -1;
            if ((tag & 7) == 0) {
                if (!varint(&p, end, &v))
                    return -1;
                if (tag >> 3 == 1)
                    id = v;
                else if (tag >> 3 == 2)
                    bytes = v;
                else if (tag >> 3 == 3)
                    sz = v;
            } else if ((tag & 7) == 2) {
                if (!varint(&p, end, &v) || v > (unsigned long)(end - p))
                    return -1;
                p += v;
            } else if ((tag & 7) == 1 || (tag & 7) == 5) {
                size_t skip = (tag & 7) == 1 ? 8 : 4;
                if ((size_t)(end - p) < skip)
                    return -1;
                p += skip;
            } else {
                return -1;
            }
        }
        at += elen;
        if (bytes > len - at)
            return -1;
        if (id == ino) {
            *off = at;
            *n = bytes;
            *size = sz;
            return 1;
        }
        at += bytes;
    }
    return 0;
}

/* ---- pstree.img ------------------------------------------------------------ */

static size_t put_varint(unsigned char *p, unsigned long v)
{
    size_t n = 0;
    do {
        p[n++] = (unsigned char)((v & 0x7f) | (v > 0x7f ? 0x80 : 0));
        v >>= 7;
    } while (v);
    return n;
}

int criu_pstree_regroup(const unsigned char *in, size_t len, unsigned long from, unsigned long to, unsigned char *out,
                        size_t cap)
{
    if (len < 8 || cap < 8)
        return -1;
    memcpy(out, in, 8); /* the image's magic */
    size_t at = 8, o = 8;
    while (at < len) {
        if (len - at < 4)
            return -1;
        size_t elen = (size_t)in[at] | (size_t)in[at + 1] << 8 | (size_t)in[at + 2] << 16 | (size_t)in[at + 3] << 24;
        at += 4;
        if (elen > len - at || cap - o < 4)
            return -1;
        size_t lenpos = o;
        o += 4;
        const unsigned char *p = in + at, *end = p + elen;
        while (p < end) { /* PstreeEntry: varint fields only (pid 1, ppid 2, pgid 3, sid 4, threads 5) */
            unsigned long tag, v;
            if (!varint(&p, end, &tag) || (tag & 7) != 0 || !varint(&p, end, &v))
                return -1;
            if (tag >> 3 == 3 && v == from)
                v = to;
            if (cap - o < 20)
                return -1;
            o += put_varint(out + o, tag);
            o += put_varint(out + o, v);
        }
        size_t n = o - lenpos - 4;
        out[lenpos] = (unsigned char)n, out[lenpos + 1] = (unsigned char)(n >> 8);
        out[lenpos + 2] = (unsigned char)(n >> 16), out[lenpos + 3] = (unsigned char)(n >> 24);
        at += elen;
    }
    return (int)o;
}
