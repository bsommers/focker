/* The supervisor ("the circle of trust"): a PR_SET_CHILD_SUBREAPER process
 * that runs the family's first command, reaps every orphan in its subtree,
 * holds fds taken over during cutovers, and serves a control socket. */
#include "focker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static struct family fam;
static int lfd = -1, sfd = -1;
static sigset_t saved_mask;
static char sockpath[PATH_MAX + 16];
static pid_t first_pid;
static int first_status = -1;
static bool linger_;

struct member *family_member(struct family *f, pid_t pid, bool create)
{
    for (int i = 0; i < f->nmembers; i++)
        if (f->members[i].pid == pid)
            return &f->members[i];
    if (!create)
        return NULL;
    if (f->nmembers == MAX_MEMBERS) { /* forget the oldest dead member */
        int i;
        for (i = 0; i < f->nmembers && f->members[i].alive; i++)
            ;
        if (i == f->nmembers)
            return NULL;
        memmove(&f->members[i], &f->members[i + 1], (size_t)(f->nmembers - i - 1) * sizeof f->members[0]);
        f->nmembers--;
    }
    struct member *m = &f->members[f->nmembers++];
    memset(m, 0, sizeof *m);
    m->pid = pid;
    m->alive = true;
    return m;
}

/* ---- setup and teardown (re-runnable so a failed handover can recover) -- */

static int setup_signals(void)
{
    static bool saved;
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGCHLD);
    sigaddset(&m, SIGTERM);
    sigaddset(&m, SIGINT);
    sigaddset(&m, SIGHUP);
    sigaddset(&m, SIGQUIT);
    if (sigprocmask(SIG_BLOCK, &m, saved ? NULL : &saved_mask) < 0)
        return -1;
    saved = true;
    signal(SIGPIPE, SIG_IGN);
    sfd = signalfd(-1, &m, SFD_CLOEXEC | SFD_NONBLOCK);
    return sfd < 0 ? -1 : 0;
}

static int setup_socket(void)
{
    struct sockaddr_un a;
    int dfd;
    if (control_addr(fam.dir, &a, &dfd) < 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        goto fail;
    unlink(sockpath);
    mode_t old = umask(0177);
    int r = bind(fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (r < 0 || listen(fd, 16) < 0)
        goto fail;
    if (dfd >= 0)
        close(dfd);
    lfd = fd;
    return 0;
fail: {
    int e = errno;
    if (fd >= 0)
        close(fd);
    if (dfd >= 0)
        close(dfd);
    errno = e;
    return -1;
}
}

static void write_file(const char *leaf, const char *text)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/%s", fam.dir, leaf);
    FILE *f = fopen(path, "we");
    if (!f)
        return;
    fputs(text, f);
    fclose(f);
}

/* ---- children ------------------------------------------------------------ */

static int live_children(void)
{
    pid_t tmp[1];
    return proc_children(getpid(), tmp, 1);
}

static void sync_members(void)
{
    pid_t kids[MAX_MEMBERS];
    int n = proc_children(getpid(), kids, MAX_MEMBERS);
    for (int i = 0; i < n; i++) {
        struct member *m = family_member(&fam, kids[i], false);
        if (m)
            continue;
        m = family_member(&fam, kids[i], true);
        if (!m)
            continue;
        m->origin = 'o'; /* the kernel handed it to us when its parent died */
        proc_cmdline(kids[i], m->cmd, sizeof m->cmd);
    }
}

static void release_stolen_for(pid_t child)
{
    for (int i = 0; i < fam.nstolen; i++) {
        struct stolen *s = &fam.stolen[i];
        if (s->child != child || s->fd < 0 || s->drain)
            continue; /* read ends drain until EOF on their own */
        close(s->fd);
        s->fd = -1;
    }
}

static void reap_children(void)
{
    for (;;) {
        siginfo_t si = { 0 };
        /* peek first so a not-yet-registered orphan's name can still be read */
        if (waitid(P_ALL, 0, &si, WEXITED | WNOHANG | WNOWAIT) < 0 || si.si_pid == 0)
            return;
        pid_t pid = si.si_pid;
        struct member *m = family_member(&fam, pid, false);
        if (!m && (m = family_member(&fam, pid, true))) {
            m->origin = 'o';
            proc_cmdline(pid, m->cmd, sizeof m->cmd);
        }
        int st;
        if (waitpid(pid, &st, WNOHANG) <= 0)
            return;
        if (m) {
            m->alive = false;
            m->status = st;
        }
        if (pid == first_pid)
            first_status = st;
        logmsg("%d (%s) %s", pid, m ? m->cmd : "?", describe_status(st));
        release_stolen_for(pid);
    }
}

static void forward_signal(int sig)
{
    pid_t kids[MAX_MEMBERS];
    int n = proc_children(getpid(), kids, MAX_MEMBERS);
    for (int i = 0; i < n; i++)
        kill(kids[i], sig);
}

static void drain(struct stolen *s)
{
    if (s->fd < 0)
        return;
    char buf[65536];
    ssize_t n = read(s->fd, buf, sizeof buf);
    if (n > 0) {
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(s->logfd, buf + off, (size_t)(n - off));
            if (w <= 0)
                break;
            off += w;
        }
        return;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
        return;
    logmsg("%s from %d closed (child %d fd %d)", s->kind, s->old_parent, s->child, s->child_fd);
    close(s->fd);
    close(s->logfd);
    s->fd = s->logfd = -1;
}

/* ---- control requests ----------------------------------------------------- */

static void origin_name(char o, char *buf, size_t len)
{
    snprintf(buf, len, "%s", o == 'r' ? "run" : o == 'a' ? "adopted" : "orphan");
}

static int do_info(struct sbuf *out)
{
    const struct member *first = family_member(&fam, first_pid, false);
    sb_printf(out, "name=%s\npid=%d\nchildren=%d\ncmd=%s\n", fam.name, getpid(), live_children(),
              first ? first->cmd : "");
    return 0;
}

static int do_members(struct sbuf *out)
{
    sync_members();
    for (int i = 0; i < fam.nmembers; i++) {
        const struct member *m = &fam.members[i];
        sb_printf(out, "%d %c %d %d\n", m->pid, m->origin, m->old_ppid, m->alive);
    }
    return 0;
}

static void inspect_json(struct sbuf *out)
{
    sb_printf(out, "{\"name\":");
    sb_json_str(out, fam.name);
    sb_printf(out, ",\"supervisor\":%d,\"state_dir\":", getpid());
    sb_json_str(out, fam.dir);
    sb_printf(out, ",\"linger\":%s,\"members\":[", linger_ ? "true" : "false");
    for (int i = 0; i < fam.nmembers; i++) {
        const struct member *m = &fam.members[i];
        char origin[16];
        origin_name(m->origin, origin, sizeof origin);
        sb_printf(out, "%s{\"pid\":%d,\"origin\":\"%s\",\"from\":", i ? "," : "", m->pid, origin);
        if (m->old_ppid)
            sb_printf(out, "%d", m->old_ppid);
        else
            sb_printf(out, "null");
        sb_printf(out, ",\"alive\":%s,\"status\":", m->alive ? "true" : "false");
        sb_json_str(out, m->alive ? "running" : describe_status(m->status));
        if (!m->alive && WIFEXITED(m->status))
            sb_printf(out, ",\"exit_code\":%d,\"signal\":null", WEXITSTATUS(m->status));
        else if (!m->alive && WIFSIGNALED(m->status))
            sb_printf(out, ",\"exit_code\":null,\"signal\":\"%s\"", signame(WTERMSIG(m->status)));
        else
            sb_printf(out, ",\"exit_code\":null,\"signal\":null");
        sb_printf(out, ",\"cmd\":");
        sb_json_str(out, m->cmd);
        sb_printf(out, "}");
    }
    sb_printf(out, "],\"held_fds\":[");
    int shown = 0;
    for (int i = 0; i < fam.nstolen; i++) {
        const struct stolen *s = &fam.stolen[i];
        if (s->fd < 0)
            continue;
        sb_printf(out, "%s{\"fd\":%d,\"kind\":\"%s\",\"child\":%d,\"child_fd\":%d,\"from\":%d,\"log\":",
                  shown++ ? "," : "", s->fd, s->kind, s->child, s->child_fd, s->old_parent);
        sb_json_str(out, s->drain ? s->log : NULL);
        sb_printf(out, "}");
    }
    sb_printf(out, "]}\n");
}

/* release FD...: close fds held for adopted children. Unknown fds are
 * reported and make the request fail, but don't stop the others. */
static int do_release(char **args, int nargs, struct sbuf *out)
{
    int rc = 0;
    for (int a = 1; a < nargs; a++) {
        int fd = atoi(args[a]);
        struct stolen *s = NULL;
        for (int i = 0; i < fam.nstolen && !s; i++)
            if (fam.stolen[i].fd >= 0 && fam.stolen[i].fd == fd)
                s = &fam.stolen[i];
        if (!s) {
            sb_printf(out, "release: focker holds no fd %d\n", fd);
            rc = 1;
            continue;
        }
        close(s->fd);
        if (s->logfd >= 0)
            close(s->logfd);
        s->fd = s->logfd = -1;
        logmsg("released %s fd %d (child %d fd %d)", s->kind, fd, s->child, s->child_fd);
        sb_printf(out, "release: closed %s fd %d (child %d fd %d)\n", s->kind, fd, s->child, s->child_fd);
    }
    return rc;
}

static int do_inspect(struct sbuf *out, bool json)
{
    sync_members();
    if (json) {
        inspect_json(out);
        return 0;
    }
    sb_printf(out, "name:       %s\nsupervisor: %d (child subreaper)\nstate dir:  %s\n", fam.name, getpid(), fam.dir);
    sb_printf(out, "linger:     %s\n\nmembers:\n", linger_ ? "yes" : "no");
    sb_printf(out, "  %-8s %-8s %-8s %-20s %s\n", "PID", "ORIGIN", "FROM", "STATE", "COMMAND");
    for (int i = 0; i < fam.nmembers; i++) {
        const struct member *m = &fam.members[i];
        char origin[16], from[16];
        origin_name(m->origin, origin, sizeof origin);
        if (m->old_ppid)
            snprintf(from, sizeof from, "%d", m->old_ppid);
        else
            snprintf(from, sizeof from, "-");
        sb_printf(out, "  %-8d %-8s %-8s %-20s %s\n", m->pid, origin, from,
                  m->alive ? "running" : describe_status(m->status), m->cmd);
    }
    sb_printf(out, "\nheld fds:\n");
    int shown = 0;
    for (int i = 0; i < fam.nstolen; i++) {
        const struct stolen *s = &fam.stolen[i];
        if (s->fd < 0)
            continue;
        if (!shown++)
            sb_printf(out, "  %-4s %-7s %-8s %-8s %-8s %s\n", "FD", "KIND", "CHILD", "CHILD-FD", "FROM", "LOG");
        sb_printf(out, "  %-4d %-7s %-8d %-8d %-8d %s\n", s->fd, s->kind, s->child, s->child_fd, s->old_parent,
                  s->drain ? s->log : "-");
    }
    if (!shown)
        sb_printf(out, "  (none)\n");
    return 0;
}

static const char *kv(char *tok, const char *key)
{
    size_t n = strlen(key);
    return strncmp(tok, key, n) == 0 && tok[n] == '=' ? tok + n + 1 : NULL;
}

static int do_cutover(char **args, int nargs, struct sbuf *out)
{
    struct cutover_opts o = { .freeze = FREEZE_PTRACE, .sig = SIGTERM, .timeout_ms = 10000,
                              .steal = true, .drain = true, .suppress = true };
    pid_t pids[MAX_PIDS];
    int npids = 0;
    for (int i = 1; i < nargs; i++) {
        const char *v;
        if ((v = kv(args[i], "freeze"))) {
            int m = parse_freeze(v);
            if (m < 0) {
                sb_printf(out, "cutover: bad freeze method '%s'\n", v);
                return 1;
            }
            o.freeze = (enum freeze_method)m;
        } else if ((v = kv(args[i], "signal"))) {
            o.sig = atoi(v);
        } else if ((v = kv(args[i], "timeout"))) {
            o.timeout_ms = atoi(v);
        } else if ((v = kv(args[i], "from"))) {
            o.from = (pid_t)atoi(v);
        } else if ((v = kv(args[i], "steal"))) {
            o.steal = atoi(v);
        } else if ((v = kv(args[i], "drain"))) {
            o.drain = atoi(v);
        } else if ((v = kv(args[i], "suppress"))) {
            o.suppress = atoi(v);
        } else if ((v = kv(args[i], "dry_run"))) {
            o.dry_run = atoi(v);
        } else if ((v = kv(args[i], "pid"))) {
            if (npids == MAX_PIDS) {
                sb_printf(out, "cutover: at most %d processes per cutover\n", MAX_PIDS);
                return 1;
            }
            pids[npids++] = (pid_t)atoi(v);
        } else {
            sb_printf(out, "cutover: unknown option '%s'\n", args[i]);
            return 1;
        }
    }
    struct cutover_job *job;
    int rc = cutover_start(&fam, &o, pids, npids, out, &job);
    while (rc == CUTOVER_PENDING) {
        rc = cutover_step(job, out);
        if (rc == CUTOVER_PENDING)
            sleep_ms(20);
    }
    if (!o.dry_run)
        logmsg("cutover %s", rc == 0 ? "succeeded" : "failed");
    return rc;
}

static int do_stop(char **args, int nargs, struct sbuf *out)
{
    int sig = SIGTERM;
    if (nargs > 1 && kv(args[1], "signal"))
        sig = atoi(kv(args[1], "signal"));
    linger_ = false;
    pid_t kids[MAX_MEMBERS];
    int n = proc_children(getpid(), kids, MAX_MEMBERS);
    for (int i = 0; i < n; i++) {
        kill(kids[i], sig);
        sb_printf(out, "sent %s to %d\n", signame(sig), kids[i]);
    }
    if (n == 0)
        sb_printf(out, "no children; %s will exit\n", fam.name);
    return 0;
}

static int resolve_exec(const char *cmd, char *out, size_t len)
{
    if (strchr(cmd, '/')) {
        snprintf(out, len, "%s", cmd);
        return access(out, X_OK);
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/local/bin:/usr/bin:/bin";
    while (*path) {
        const char *colon = strchrnul(path, ':');
        int n = (int)(colon - path);
        snprintf(out, len, "%.*s/%s", n ? n : 1, n ? path : ".", cmd);
        if (access(out, X_OK) == 0)
            return 0;
        path = *colon ? colon + 1 : colon;
    }
    errno = ENOENT;
    return -1;
}

/* Replace this process with a new parent program. execve keeps the PID, so
 * every child focker adopted stays a real child of the new program, and the
 * held fds stay open across exec once FD_CLOEXEC is cleared. */
static int do_handover(char **args, int nargs, int cfd, struct sbuf *out)
{
    if (nargs < 2) {
        sb_printf(out, "handover: missing command\n");
        return 1;
    }
    char **argv = args + 1;
    char path[PATH_MAX];
    if (resolve_exec(argv[0], path, sizeof path) < 0) {
        sb_printf(out, "handover: cannot run %s: %s\n", argv[0], strerror(errno));
        return 1;
    }

    static struct sbuf kids, fds;
    kids.len = fds.len = 0;
    pid_t k[MAX_MEMBERS];
    int nk = proc_children(getpid(), k, MAX_MEMBERS);
    sb_printf(&kids, "FOCKER_CHILDREN=");
    for (int i = 0; i < nk; i++)
        sb_printf(&kids, "%s%d", i ? "," : "", k[i]);
    sb_printf(&fds, "FOCKER_FDS=");
    int nf = 0;
    for (int i = 0; i < fam.nstolen; i++) {
        const struct stolen *s = &fam.stolen[i];
        if (s->fd >= 0)
            sb_printf(&fds, "%s%d:%d:%s:%d", nf++ ? "," : "", s->fd, s->child, s->kind, s->child_fd);
    }
    char name[96], dir[PATH_MAX + 32];
    snprintf(name, sizeof name, "FOCKER_NAME=%s", fam.name);
    snprintf(dir, sizeof dir, "FOCKER_STATE_DIR=%s", fam.dir);

    int nenv = 0;
    for (char **e = environ; *e; e++)
        nenv++;
    char **envp = calloc((size_t)nenv + 5, sizeof *envp);
    if (!envp) {
        sb_printf(out, "handover: out of memory\n");
        return 1;
    }
    int j = 0;
    for (char **e = environ; *e; e++)
        if (strncmp(*e, "FOCKER_CHILDREN=", 16) && strncmp(*e, "FOCKER_FDS=", 11) &&
            strncmp(*e, "FOCKER_NAME=", 12) && strncmp(*e, "FOCKER_STATE_DIR=", 17))
            envp[j++] = *e;
    envp[j++] = kids.p;
    envp[j++] = fds.p;
    envp[j++] = name;
    envp[j++] = dir;

    /* "exec" instead of "ok": cfd is close-on-exec, so a successful execve
     * closes it and the client takes the EOF as success; a failed one
     * appends "err" and the reason below */
    sb_printf(out, "handover: %d becomes %s; it inherits %d child%s and %d fd%s\n", getpid(), path, nk,
              nk == 1 ? "" : "ren", nf, nf == 1 ? "" : "s");
    dprintf(cfd, "exec\n%.*s", (int)out->len, out->p);
    out->len = 0;
    logmsg("handing over to %s", path);
    char note[PATH_MAX + 64];
    snprintf(note, sizeof note, "handed over to %s\n", path);
    write_file("exit", note);

    close(lfd);
    lfd = -1;
    unlink(sockpath);
    close(sfd);
    sfd = -1;
    for (int i = 0; i < fam.nstolen; i++) {
        struct stolen *s = &fam.stolen[i];
        if (s->fd >= 0)
            fcntl(s->fd, F_SETFD, 0);
        if (s->logfd >= 0) {
            close(s->logfd);
            s->logfd = -1;
        }
    }
    signal(SIGPIPE, SIG_DFL);
    sigprocmask(SIG_SETMASK, &saved_mask, NULL);
    fflush(NULL);

    execve(path, argv, envp);

    /* exec failed after the point of no return for the client: recover */
    int e = errno;
    free(envp);
    logmsg("handover to %s failed: %s; the family stays with focker", path, strerror(e));
    char unlink_path[PATH_MAX + 32];
    snprintf(unlink_path, sizeof unlink_path, "%s/exit", fam.dir);
    unlink(unlink_path);
    setup_signals();
    if (setup_socket() < 0)
        logmsg("cannot re-create control socket: %s", strerror(errno));
    for (int i = 0; i < fam.nstolen; i++) {
        struct stolen *s = &fam.stolen[i];
        if (s->fd >= 0)
            fcntl(s->fd, F_SETFD, FD_CLOEXEC);
        if (s->fd >= 0 && s->drain)
            s->logfd = open(s->log, O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    dprintf(cfd, "err\nhandover: cannot exec %s: %s; the family stays with focker\n", path, strerror(e));
    close(cfd);
    return -1; /* reply sent */
}

static void handle_client(int cfd)
{
    struct ucred cr;
    socklen_t cl = sizeof cr;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) < 0 || cr.uid != getuid()) {
        close(cfd);
        return;
    }
    struct timeval tv = { 5, 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    static char buf[65536];
    size_t len = 0;
    for (;;) {
        ssize_t n = read(cfd, buf + len, sizeof buf - 1 - len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        len += (size_t)n;
        if (len == sizeof buf - 1)
            break;
    }
    buf[len] = '\0';
    char *args[MAX_ARGS + 1];
    int nargs = 0;
    for (size_t off = 0; off < len && nargs < MAX_ARGS; off += strlen(buf + off) + 1)
        args[nargs++] = buf + off;
    args[nargs] = NULL;

    struct sbuf out = { 0 };
    int rc = 1;
    if (nargs == 0)
        sb_printf(&out, "empty request\n");
    else if (strcmp(args[0], "info") == 0)
        rc = do_info(&out);
    else if (strcmp(args[0], "members") == 0)
        rc = do_members(&out);
    else if (strcmp(args[0], "inspect") == 0)
        rc = do_inspect(&out, nargs > 1 && strcmp(args[1], "json=1") == 0);
    else if (strcmp(args[0], "cutover") == 0)
        rc = do_cutover(args, nargs, &out);
    else if (strcmp(args[0], "release") == 0)
        rc = do_release(args, nargs, &out);
    else if (strcmp(args[0], "stop") == 0)
        rc = do_stop(args, nargs, &out);
    else if (strcmp(args[0], "handover") == 0) {
        rc = do_handover(args, nargs, cfd, &out);
        if (rc < 0) { /* reply already sent and cfd closed */
            sb_free(&out);
            return;
        }
    } else
        sb_printf(&out, "unknown request '%s'\n", args[0]);

    dprintf(cfd, "%s\n%.*s", rc == 0 ? "ok" : "err", (int)out.len, out.p ? out.p : "");
    close(cfd);
    sb_free(&out);
}

/* ---- main loop ------------------------------------------------------------ */

int supervisor_run(const char *name, const char *dir, char **argv, bool detached, bool linger, int ready_fd)
{
    static char prefix[NAME_MAX_LEN + 16];
    snprintf(prefix, sizeof prefix, "focker[%s]", name);
    log_prefix = prefix;
    snprintf(fam.name, sizeof fam.name, "%s", name);
    snprintf(fam.dir, sizeof fam.dir, "%s", dir);
    snprintf(sockpath, sizeof sockpath, "%s/control.sock", dir);
    linger_ = linger;

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        logmsg("cannot become a child subreaper: %s", strerror(errno));
        return FK_FAIL;
    }
    if (setup_signals() < 0 || setup_socket() < 0) {
        logmsg("cannot set up control socket %s: %s", sockpath, strerror(errno));
        return FK_FAIL;
    }

    int errpipe[2];
    if (pipe2(errpipe, O_CLOEXEC) < 0) {
        logmsg("pipe: %s", strerror(errno));
        return FK_FAIL;
    }
    first_pid = fork();
    if (first_pid < 0) {
        logmsg("fork: %s", strerror(errno));
        return FK_FAIL;
    }
    if (first_pid == 0) {
        close(errpipe[0]);
        if (detached)
            setpgid(0, 0); /* own group, parented inside focker's session: never an orphaned pgrp */
        signal(SIGPIPE, SIG_DFL);
        sigprocmask(SIG_SETMASK, &saved_mask, NULL);
        execvp(argv[0], argv);
        int e = errno;
        if (write(errpipe[1], &e, sizeof e) < 0)
            _exit(127);
        _exit(127);
    }
    close(errpipe[1]);
    int child_errno = 0;
    ssize_t got;
    while ((got = read(errpipe[0], &child_errno, sizeof child_errno)) < 0 && errno == EINTR)
        ;
    close(errpipe[0]);
    if (got > 0) {
        waitpid(first_pid, NULL, 0);
        logmsg("cannot run %s: %s", argv[0], strerror(child_errno));
        unlink(sockpath);
        return 127;
    }

    struct member *m = family_member(&fam, first_pid, true);
    m->origin = 'r';
    proc_cmdline(first_pid, m->cmd, sizeof m->cmd);
    if (!m->cmd[0] || m->cmd[0] == '[')
        snprintf(m->cmd, sizeof m->cmd, "%s", argv[0]);

    char text[256];
    snprintf(text, sizeof text, "%d\n", getpid());
    write_file("pid", text);
    snprintf(text, sizeof text, "%s\n", m->cmd);
    write_file("cmd", text);
    logmsg("supervisor %d started %d (%s)", getpid(), first_pid, m->cmd);
    if (ready_fd >= 0) {
        if (write(ready_fd, "1", 1) < 0) { /* launcher went away; carry on */ }
        close(ready_fd);
    }

    for (;;) {
        reap_children();
        if (!linger_ && live_children() == 0)
            break;

        struct pollfd pfd[2 + MAX_STOLEN];
        int idx[2 + MAX_STOLEN];
        nfds_t n = 0;
        pfd[n++] = (struct pollfd){ .fd = sfd, .events = POLLIN };
        pfd[n++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
        for (int i = 0; i < fam.nstolen; i++)
            if (fam.stolen[i].fd >= 0 && fam.stolen[i].drain) {
                idx[n] = i;
                pfd[n++] = (struct pollfd){ .fd = fam.stolen[i].fd, .events = POLLIN };
            }
        if (poll(pfd, n, 1000) < 0 && errno != EINTR) {
            logmsg("poll: %s", strerror(errno));
            break;
        }
        if (pfd[0].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(sfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGCHLD)
                    continue; /* reaped at the top of the loop */
                /* the tty already signals the whole foreground group; only relay kill(1)-style signals */
                if (si.ssi_code != SI_KERNEL) {
                    logmsg("got %s, forwarding to children", signame((int)si.ssi_signo));
                    forward_signal((int)si.ssi_signo);
                }
                linger_ = false;
            }
        }
        if (pfd[1].revents & POLLIN) {
            int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            if (cfd >= 0) {
                handle_client(cfd);
                /* a cutover or release may have closed or moved held-fd slots, so
                 * idx[] is stale; poll is level-triggered, so drain next round */
                continue;
            }
        }
        for (nfds_t i = 2; i < n; i++)
            if (pfd[i].revents)
                drain(&fam.stolen[idx[i]]);
    }

    int code = 0;
    if (first_status >= 0)
        code = WIFEXITED(first_status) ? WEXITSTATUS(first_status)
             : WIFSIGNALED(first_status) ? 128 + WTERMSIG(first_status) : 1;
    char exit_text[128];
    snprintf(exit_text, sizeof exit_text, "%s\n", first_status >= 0 ? describe_status(first_status) : "exited");
    write_file("exit", exit_text);
    if (lfd >= 0)
        close(lfd);
    unlink(sockpath);
    logmsg("family empty, exiting with %d", code);
    return code;
}
