/* The cutover: freeze -> take over fds -> cut ties -> verify -> thaw.
 *
 * Linux never lets a live parent give a child away. The only time the kernel
 * rewrites real_parent is when the parent dies, and it then picks the nearest
 * ancestor flagged PR_SET_CHILD_SUBREAPER. focker is that ancestor, so the
 * cutover is "make the old parent die without the child noticing". */
#include "focker.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#define SIGBIT(s) (1ULL << ((s) - 1))
/* routine traffic that is never swallowed, even if it arrives mid-cutover */
#define BENIGN (SIGBIT(SIGCHLD) | SIGBIT(SIGCONT) | SIGBIT(SIGWINCH) | SIGBIT(SIGURG))

struct frozen {
    pid_t    pid;
    pid_t    tids[MAX_TIDS];
    int      inject[MAX_TIDS]; /* signal caught in signal-delivery-stop, re-sent on detach */
    int      ntids;
    uint64_t pending_before;
    bool     frozen;
};

struct plan_fd {
    pid_t child, parent;
    int   parent_fd, child_fd;
    bool  drain;
    char  kind[8];
};

int parse_freeze(const char *s)
{
    if (strcmp(s, "ptrace") == 0)
        return FREEZE_PTRACE;
    if (strcmp(s, "signal") == 0)
        return FREEZE_SIGNAL;
    if (strcmp(s, "none") == 0)
        return FREEZE_NONE;
    return -1;
}

const char *freeze_name(enum freeze_method m)
{
    return m == FREEZE_PTRACE ? "ptrace" : m == FREEZE_SIGNAL ? "signal" : "none";
}

static const char *cmd_of(pid_t pid)
{
    static char buf[4][96];
    static int slot;
    char *b = buf[slot++ % 4];
    if (proc_cmdline(pid, b, sizeof buf[0]) < 0)
        snprintf(b, sizeof buf[0], "?");
    return b;
}

/* ---- ptrace freezing --------------------------------------------------- */

/* Wait for tid to be in a ptrace-stop. A signal-delivery-stop counts too: the
 * thread is stopped, and we remember the signal so it is not lost. */
static int wait_ptrace_stop(pid_t tid, int *inject)
{
    for (;;) {
        int st;
        if (waitpid(tid, &st, __WALL) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!WIFSTOPPED(st))
            return -1; /* thread exited */
        if ((st >> 16) != PTRACE_EVENT_STOP)
            *inject = WSTOPSIG(st);
        return 0;
    }
}

static bool has_tid(const struct frozen *z, pid_t tid)
{
    for (int i = 0; i < z->ntids; i++)
        if (z->tids[i] == tid)
            return true;
    return false;
}

static int freeze_ptrace(struct frozen *z, struct sbuf *out)
{
    /* Re-list until stable: a thread may clone() between listing and seizing. */
    for (int round = 0; round < 16; round++) {
        pid_t tids[MAX_TIDS];
        int n = proc_threads(z->pid, tids, MAX_TIDS);
        if (n <= 0) {
            sb_printf(out, "cutover: %d vanished while freezing\n", z->pid);
            return -1;
        }
        bool added = false;
        for (int i = 0; i < n; i++) {
            if (has_tid(z, tids[i]))
                continue;
            if (z->ntids == MAX_TIDS) {
                sb_printf(out, "cutover: %d has more than %d threads\n", z->pid, MAX_TIDS);
                return -1;
            }
            if (ptrace(PTRACE_SEIZE, tids[i], NULL, NULL) < 0) {
                if (errno == ESRCH)
                    continue;
                sb_printf(out, "cutover: cannot ptrace %d: %s (another tracer? check kernel.yama.ptrace_scope)\n",
                          tids[i], strerror(errno));
                return -1;
            }
            int inj = 0;
            z->tids[z->ntids] = tids[i];
            z->inject[z->ntids] = 0;
            z->ntids++;
            added = true;
            if (ptrace(PTRACE_INTERRUPT, tids[i], NULL, NULL) < 0 || wait_ptrace_stop(tids[i], &inj) < 0) {
                z->ntids--; /* thread exited under us */
                continue;
            }
            z->inject[z->ntids - 1] = inj;
        }
        if (!added)
            break;
    }
    z->frozen = z->ntids > 0;
    return z->frozen ? 0 : -1;
}

static void note_swallowed(struct sbuf *out, pid_t pid, int sig)
{
    sb_printf(out, "cutover: swallowed %s sent to %d during the cutover\n", signame(sig), pid);
}

/* Detach from every thread. With swallow != 0, first let the threads run under
 * trace so signals that arrived mid-cutover (a PR_SET_PDEATHSIG notice, or the
 * old parent's "kill my children" on shutdown) reach a signal-delivery-stop and
 * can be dropped. */
static void thaw_ptrace(struct frozen *z, uint64_t swallow, struct sbuf *out)
{
    bool live[MAX_TIDS];
    for (int i = 0; i < z->ntids; i++)
        live[i] = true;

    if (swallow) {
        for (int i = 0; i < z->ntids; i++) {
            int s = z->inject[i];
            if (s && (swallow & SIGBIT(s))) {
                note_swallowed(out, z->pid, s);
                swallow &= ~SIGBIT(s);
                s = 0;
            }
            z->inject[i] = 0;
            if (ptrace(PTRACE_CONT, z->tids[i], NULL, (void *)(long)s) < 0)
                live[i] = false;
        }
        long deadline = now_ms() + 250;
        while (swallow && now_ms() < deadline) {
            for (int i = 0; i < z->ntids; i++) {
                if (!live[i])
                    continue;
                int st;
                pid_t r = waitpid(z->tids[i], &st, __WALL | WNOHANG);
                if (r == 0)
                    continue;
                if (r < 0 || !WIFSTOPPED(st)) {
                    live[i] = false;
                    continue;
                }
                int s = WSTOPSIG(st);
                if ((st >> 16) == PTRACE_EVENT_STOP) {
                    s = 0; /* our leftover interrupt trap */
                } else if (swallow & SIGBIT(s)) {
                    note_swallowed(out, z->pid, s);
                    swallow &= ~SIGBIT(s);
                    s = 0;
                }
                ptrace(PTRACE_CONT, z->tids[i], NULL, (void *)(long)s);
            }
            sleep_ms(2);
        }
        /* stop everyone again: PTRACE_DETACH needs a stopped tracee */
        for (int i = 0; i < z->ntids; i++) {
            if (!live[i])
                continue;
            if (ptrace(PTRACE_INTERRUPT, z->tids[i], NULL, NULL) < 0 ||
                wait_ptrace_stop(z->tids[i], &z->inject[i]) < 0) {
                live[i] = false;
                continue;
            }
            int s = z->inject[i];
            if (s && (swallow & SIGBIT(s))) {
                note_swallowed(out, z->pid, s);
                swallow &= ~SIGBIT(s);
                z->inject[i] = 0;
            }
        }
        for (int s = 1; s < 64; s++)
            if (swallow & SIGBIT(s))
                sb_printf(out, "cutover: %s for %d is still pending (blocked by the process?) and was not swallowed\n",
                          signame(s), z->pid);
    }
    for (int i = 0; i < z->ntids; i++)
        if (live[i])
            ptrace(PTRACE_DETACH, z->tids[i], NULL, (void *)(long)z->inject[i]);
    z->frozen = false;
}

/* ---- signal freezing --------------------------------------------------- */

static int freeze_signal(struct frozen *z, struct sbuf *out)
{
    if (kill(z->pid, SIGSTOP) < 0) {
        sb_printf(out, "cutover: cannot stop %d: %s\n", z->pid, strerror(errno));
        return -1;
    }
    for (long deadline = now_ms() + 2000; now_ms() < deadline; sleep_ms(2)) {
        char s = proc_state(z->pid);
        if (s == 'T' || s == 't') {
            z->frozen = true;
            return 0;
        }
        if (s == 0 || s == 'Z')
            break;
    }
    sb_printf(out, "cutover: %d did not stop\n", z->pid);
    kill(z->pid, SIGCONT);
    return -1;
}

static int freeze(struct frozen *z, enum freeze_method m, struct sbuf *out)
{
    if (m == FREEZE_PTRACE)
        return freeze_ptrace(z, out);
    if (m == FREEZE_SIGNAL)
        return freeze_signal(z, out);
    return 0;
}

static void thaw(struct frozen *z, enum freeze_method m, uint64_t swallow, struct sbuf *out)
{
    if (!z->frozen)
        return;
    if (m == FREEZE_PTRACE) {
        thaw_ptrace(z, swallow, out);
    } else if (m == FREEZE_SIGNAL) {
        kill(z->pid, SIGCONT);
        z->frozen = false;
    }
}

/* ---- fd takeover -------------------------------------------------------- */

/* Find the old parent's fds that are the other end of something the child
 * holds: a pipe with the opposite access mode, or a unix socket whose peer is
 * the child's socket. Those are the ties that would break (EOF, EPIPE, a full
 * pipe that blocks the child forever) when the old parent dies. */
static int plan_fds(pid_t child, pid_t parent, struct plan_fd *plan, int max)
{
    static struct fdent cf[MAX_FDENTS], pf[MAX_FDENTS];
    int nc = proc_fds(child, cf, MAX_FDENTS);
    int np = proc_fds(parent, pf, MAX_FDENTS);
    if (nc < 0 || np < 0)
        return -1;
    int n = 0;
    for (int i = 0; i < np && n < max; i++) {
        if (pf[i].kind == 'o')
            continue;
        for (int j = 0; j < nc; j++) {
            if (cf[j].kind != pf[i].kind)
                continue;
            bool linked;
            if (pf[i].kind == 'p') /* same pipe, opposite ends */
                linked = cf[j].ino == pf[i].ino && cf[j].accmode != pf[i].accmode;
            else                   /* the two ends of a unix socket */
                linked = unixdiag_peer(pf[i].ino) == cf[j].ino && cf[j].ino != 0;
            if (!linked)
                continue;
            struct plan_fd *p = &plan[n++];
            p->child = child;
            p->parent = parent;
            p->parent_fd = pf[i].fd;
            p->child_fd = cf[j].fd;
            if (pf[i].kind == 'p') {
                p->drain = pf[i].accmode == O_RDONLY;
                snprintf(p->kind, sizeof p->kind, "%s", p->drain ? "pipe-r" : "pipe-w");
            } else {
                p->drain = true;
                snprintf(p->kind, sizeof p->kind, "unix");
            }
            break;
        }
    }
    return n;
}

static void plan_line(struct sbuf *out, const char *verb, const struct plan_fd *p)
{
    sb_printf(out, "%s %s fd %d of %d (child %d fd %d)%s\n", verb, p->kind, p->parent_fd, p->parent,
              p->child, p->child_fd, p->drain ? ", draining its output to a log" : ", held open");
}

/* ---- the cutover -------------------------------------------------------- */

static int index_of(const pid_t *v, int n, pid_t pid)
{
    for (int i = 0; i < n; i++)
        if (v[i] == pid)
            return i;
    return -1;
}

int cutover(struct family *f, const struct cutover_opts *o, const pid_t *req, int nreq, struct sbuf *out)
{
    pid_t self = getpid();
    pid_t v[MAX_PIDS], op[MAX_PIDS];
    int nv = 0;

    /* 0. collect and validate targets */
    if (o->from) {
        if (o->from == self || !proc_is_descendant(o->from, self)) {
            sb_printf(out, "cutover: %d is not a descendant of this family\n", o->from);
            return 1;
        }
        nv = proc_children(o->from, v, MAX_PIDS);
        if (nv <= 0) {
            sb_printf(out, "cutover: %d has no children\n", o->from);
            return 1;
        }
    }
    for (int i = 0; i < nreq; i++) {
        if (index_of(v, nv, req[i]) >= 0)
            continue;
        if (nv == MAX_PIDS) {
            sb_printf(out, "cutover: at most %d processes per cutover\n", MAX_PIDS);
            return 1;
        }
        v[nv++] = req[i];
    }
    if (nv == 0) {
        sb_printf(out, "cutover: nothing to cut over\n");
        return 1;
    }

    int keep = 0;
    for (int i = 0; i < nv; i++) {
        pid_t pid = v[i], pp = proc_ppid(pid);
        if (pid == self) {
            sb_printf(out, "cutover: %d is focker itself\n", pid);
            return 1;
        }
        if (pp < 0) {
            sb_printf(out, "cutover: no such process %d\n", pid);
            return 1;
        }
        if (pp == self) {
            sb_printf(out, "cutover: %d is already a child of focker, skipping\n", pid);
            continue;
        }
        if (!proc_is_descendant(pid, self)) {
            sb_printf(out, "cutover: %d is not in family %s (focker can only adopt its own descendants;"
                           " start the old parent with 'focker run')\n", pid, f->name);
            return 1;
        }
        v[keep] = pid;
        op[keep] = pp;
        keep++;
    }
    nv = keep;
    if (nv == 0)
        return 0;
    for (int i = 0; i < nv; i++) {
        int j = index_of(v, nv, op[i]);
        if (j >= 0) {
            sb_printf(out, "cutover: %d is both a target and the parent of %d; cut over one generation at a time\n",
                      v[j], v[i]);
            return 1;
        }
    }

    pid_t parents[MAX_PIDS];
    int nparents = 0;
    for (int i = 0; i < nv; i++)
        if (index_of(parents, nparents, op[i]) < 0)
            parents[nparents++] = op[i];

    /* bystanders: the old parents' other children get orphaned too */
    pid_t by[MAX_PIDS];
    int nby = 0;
    for (int k = 0; k < nparents; k++) {
        pid_t kids[MAX_PIDS];
        int nk = proc_children(parents[k], kids, MAX_PIDS);
        for (int i = 0; i < nk && nby < MAX_PIDS; i++)
            if (index_of(v, nv, kids[i]) < 0)
                by[nby++] = kids[i];
    }

    static struct plan_fd plan[MAX_STOLEN];
    int nplan = 0;
    if (o->steal) {
        if (unixdiag_load() < 0)
            sb_printf(out, "cutover: cannot read unix socket peers (%s); only pipes will be matched\n",
                      strerror(errno));
        for (int i = 0; i < nv; i++) {
            int n = plan_fds(v[i], op[i], plan + nplan, MAX_STOLEN - nplan);
            if (n > 0)
                nplan += n;
        }
    }

    if (o->dry_run) {
        for (int i = 0; i < nv; i++)
            sb_printf(out, "plan: adopt %d (%s) from %d (%s)\n", v[i], cmd_of(v[i]), op[i], cmd_of(op[i]));
        for (int i = 0; i < nplan; i++)
            plan_line(out, "plan: take over", &plan[i]);
        for (int i = 0; i < nby; i++)
            sb_printf(out, "plan: %d (%s) is also a child of the old parent and will be adopted unfrozen\n",
                      by[i], cmd_of(by[i]));
        for (int k = 0; k < nparents; k++) {
            if (o->sig)
                sb_printf(out, "plan: send %s to old parent %d\n", signame(o->sig), parents[k]);
            else
                sb_printf(out, "plan: wait for old parent %d to exit\n", parents[k]);
        }
        sb_printf(out, "plan: freeze=%s, timeout=%.1fs\n", freeze_name(o->freeze), o->timeout_ms / 1000.0);
        return 0;
    }

    int rc = 1;
    int stolen_base = f->nstolen;
    int pidfds[MAX_PIDS];
    for (int k = 0; k < nparents; k++)
        pidfds[k] = -1;
    struct frozen *z = calloc((size_t)nv, sizeof *z);
    if (!z) {
        sb_printf(out, "cutover: out of memory\n");
        return 1;
    }

    /* 1. freeze */
    for (int i = 0; i < nv; i++) {
        z[i].pid = v[i];
        z[i].pending_before = proc_sigpending(v[i]);
        if (freeze(&z[i], o->freeze, out) < 0)
            goto rollback;
        if (o->freeze == FREEZE_PTRACE)
            sb_printf(out, "cutover: froze %d (%s, %d thread%s, ptrace)\n", v[i], cmd_of(v[i]), z[i].ntids,
                      z[i].ntids == 1 ? "" : "s");
        else if (o->freeze == FREEZE_SIGNAL)
            sb_printf(out, "cutover: froze %d (%s, SIGSTOP)\n", v[i], cmd_of(v[i]));
    }

    /* 2. take over the old parents' ends of the child's pipes and sockets */
    for (int k = 0; k < nparents; k++) {
        pidfds[k] = pidfd_open_(parents[k]);
        if (pidfds[k] < 0) {
            sb_printf(out, "cutover: cannot open old parent %d: %s\n", parents[k], strerror(errno));
            goto rollback;
        }
    }
    for (int i = 0; i < nplan; i++) {
        const struct plan_fd *p = &plan[i];
        if (f->nstolen == MAX_STOLEN) {
            sb_printf(out, "cutover: holding %d fds already; not taking more\n", MAX_STOLEN);
            break;
        }
        int nfd = pidfd_getfd_(pidfds[index_of(parents, nparents, p->parent)], p->parent_fd);
        if (nfd < 0) {
            sb_printf(out, "cutover: cannot take fd %d of %d: %s\n", p->parent_fd, p->parent, strerror(errno));
            continue;
        }
        struct stolen *s = &f->stolen[f->nstolen++];
        memset(s, 0, sizeof *s);
        s->fd = nfd;
        s->logfd = -1;
        s->child = p->child;
        s->old_parent = p->parent;
        s->parent_fd = p->parent_fd;
        s->child_fd = p->child_fd;
        s->drain = p->drain && o->drain;
        snprintf(s->kind, sizeof s->kind, "%s", p->kind);
        if (s->drain) {
            snprintf(s->log, sizeof s->log, "%s/adopted-%d-fd%d.log", f->dir, p->child, p->child_fd);
            s->logfd = open(s->log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
            if (s->logfd < 0) {
                sb_printf(out, "cutover: cannot open %s: %s; holding the fd without draining\n", s->log,
                          strerror(errno));
                s->drain = false;
                s->log[0] = '\0';
            }
        }
        struct plan_fd shown = *p;
        shown.drain = s->drain;
        plan_line(out, "cutover: took over", &shown);
    }

    /* 3. cut ties */
    for (int k = 0; k < nparents; k++) {
        if (!o->sig) {
            sb_printf(out, "cutover: waiting for old parent %d to exit\n", parents[k]);
        } else if (pidfd_signal_(pidfds[k], o->sig) < 0 && errno != ESRCH) {
            sb_printf(out, "cutover: cannot signal old parent %d: %s\n", parents[k], strerror(errno));
            goto rollback;
        } else {
            sb_printf(out, "cutover: sent %s to old parent %d\n", signame(o->sig), parents[k]);
        }
    }
    long t0 = now_ms();
    bool moved = false;
    for (;;) {
        moved = true;
        for (int i = 0; i < nv; i++)
            if (proc_ppid(v[i]) == op[i]) {
                moved = false;
                break;
            }
        if (moved || now_ms() - t0 > o->timeout_ms)
            break;
        struct pollfd pfd[MAX_PIDS];
        for (int k = 0; k < nparents; k++)
            pfd[k] = (struct pollfd){ .fd = pidfds[k], .events = POLLIN };
        poll(pfd, (nfds_t)nparents, 20);
    }
    if (!moved) {
        sb_printf(out, "cutover: timed out after %.1fs: the old parent is still alive; nothing was moved\n",
                  o->timeout_ms / 1000.0);
        goto rollback;
    }
    long dt = now_ms() - t0;

    /* 4. verify */
    int failures = 0;
    for (int i = 0; i < nv; i++) {
        pid_t pp = proc_ppid(v[i]);
        if (pp == self) {
            struct member *m = family_member(f, v[i], true);
            if (m) {
                m->origin = 'a';
                m->old_ppid = op[i];
                m->alive = true;
                proc_cmdline(v[i], m->cmd, sizeof m->cmd);
            }
            sb_printf(out, "cutover: adopted %d (ppid %d -> %d) in %ld ms\n", v[i], op[i], self, dt);
        } else if (pp < 0 || proc_state(v[i]) == 'Z') {
            sb_printf(out, "cutover: %d died during the cutover (PR_SET_PDEATHSIG=SIGKILL cannot be intercepted)\n",
                      v[i]);
            failures++;
        } else {
            sb_printf(out, "cutover: %d went to %d, not focker: another subreaper sits between them\n", v[i], pp);
            failures++;
        }
    }

    /* 5. thaw, swallowing whatever was aimed at the child while it was frozen */
    for (int i = 0; i < nv; i++) {
        if (!z[i].frozen)
            continue;
        uint64_t fresh = (proc_sigpending(v[i]) & ~z[i].pending_before) & ~BENIGN;
        if (o->freeze == FREEZE_SIGNAL)
            for (int s = 1; s < 64; s++)
                if (fresh & SIGBIT(s) && s != SIGSTOP)
                    sb_printf(out, "cutover: warning: %s reached %d while it was stopped and will be delivered"
                                   " now (--freeze ptrace would swallow it)\n", signame(s), v[i]);
        thaw(&z[i], o->freeze, o->suppress ? fresh : 0, out);
        sb_printf(out, "cutover: thawed %d\n", v[i]);
    }

    /* 6. report bystanders */
    for (int i = 0; i < nby; i++) {
        if (proc_ppid(by[i]) != self)
            continue;
        struct member *m = family_member(f, by[i], true);
        if (m) {
            m->origin = 'a';
            m->alive = true;
            proc_cmdline(by[i], m->cmd, sizeof m->cmd);
            for (int k = 0; k < nparents; k++)
                m->old_ppid = parents[k]; /* best effort when several parents were cut */
        }
        sb_printf(out, "cutover: also adopted %d (%s), orphaned by the same exit\n", by[i], cmd_of(by[i]));
    }
    rc = failures ? 1 : 0;
    goto out;

rollback:
    for (int i = f->nstolen - 1; i >= stolen_base; i--) {
        close(f->stolen[i].fd);
        if (f->stolen[i].logfd >= 0)
            close(f->stolen[i].logfd);
    }
    f->nstolen = stolen_base;
    for (int i = 0; i < nv; i++)
        thaw(&z[i], o->freeze, 0, out);
    sb_printf(out, "cutover: rolled back; processes left with their original parents\n");
out:
    for (int k = 0; k < nparents; k++)
        if (pidfds[k] >= 0)
            close(pidfds[k]);
    free(z);
    return rc;
}
