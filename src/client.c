/* Client side: talk to a family's control socket, and the read-only commands
 * (ps, tree, logs) that mostly need /proc and the state directory. */
#include "focker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int client_request(const char *name, char **args, int nargs, struct sbuf *reply)
{
    char dir[PATH_MAX];
    if (family_dir(name, dir, sizeof dir) < 0)
        return -1;
    struct sockaddr_un a;
    int dfd;
    if (control_addr(dir, &a, &dfd) < 0)
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        if (dfd >= 0)
            close(dfd);
        errno = e;
        return -1;
    }
    if (dfd >= 0)
        close(dfd);
    for (int i = 0; i < nargs; i++) {
        size_t n = strlen(args[i]) + 1;
        if (send(fd, args[i], n, MSG_NOSIGNAL) != (ssize_t)n) {
            close(fd);
            return -1;
        }
    }
    shutdown(fd, SHUT_WR);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            sb_printf(reply, "%.*s", (int)n, buf);
    close(fd);
    if (!reply->p) {
        errno = ECONNRESET;
        return -1;
    }
    char *nl = strchr(reply->p, '\n');
    if (!nl) {
        errno = EPROTO;
        return -1;
    }
    int rc = strncmp(reply->p, "ok\n", 3) == 0 ? 0 : 1;
    size_t skip = (size_t)(nl - reply->p) + 1;
    memmove(reply->p, reply->p + skip, reply->len - skip + 1);
    reply->len -= skip;
    return rc;
}

static const char *field(const char *text, const char *key, char *out, size_t len)
{
    size_t k = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *nl = strchr(line, '\n');
        if (strncmp(line, key, k) == 0 && line[k] == '=') {
            size_t n = nl ? (size_t)(nl - line - k - 1) : strlen(line + k + 1);
            if (n >= len)
                n = len - 1;
            memcpy(out, line + k + 1, n);
            out[n] = '\0';
            return out;
        }
        line = nl ? nl + 1 : NULL;
    }
    out[0] = '\0';
    return NULL;
}

int client_info(const char *name, pid_t *pid, int *nchildren, char *cmd, size_t cmdlen)
{
    struct sbuf r = { 0 };
    char *args[] = { "info" };
    int rc = client_request(name, args, 1, &r);
    if (rc != 0) {
        sb_free(&r);
        return -1;
    }
    char v[64];
    if (pid)
        *pid = (pid_t)atoi(field(r.p, "pid", v, sizeof v) ? v : "0");
    if (nchildren)
        *nchildren = atoi(field(r.p, "children", v, sizeof v) ? v : "0");
    if (cmd)
        field(r.p, "cmd", cmd, cmdlen);
    sb_free(&r);
    return 0;
}

static void read_leaf(const char *dir, const char *leaf, char *out, size_t len)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    out[0] = '\0';
    FILE *f = fopen(path, "re");
    if (!f)
        return;
    if (fgets(out, (int)len, f))
        out[strcspn(out, "\n")] = '\0';
    fclose(f);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int cmd_ps(bool all)
{
    char rt[PATH_MAX];
    if (runtime_dir(rt, sizeof rt) < 0) {
        fprintf(stderr, "focker: cannot open runtime dir: %s\n", strerror(errno));
        return FK_FAIL;
    }
    DIR *d = opendir(rt);
    if (!d) {
        fprintf(stderr, "focker: %s: %s\n", rt, strerror(errno));
        return FK_FAIL;
    }
    char *names[1024];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 1024)
        if (valid_name(e->d_name))
            names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_str);

    printf("%-24s %-8s %-18s %-9s %s\n", "NAME", "PID", "STATUS", "CHILDREN", "COMMAND");
    for (int i = 0; i < n; i++) {
        pid_t pid;
        int kids;
        char cmd[256];
        if (client_info(names[i], &pid, &kids, cmd, sizeof cmd) == 0) {
            printf("%-24s %-8d %-18s %-9d %s\n", names[i], pid, "running", kids, cmd);
        } else if (all) {
            char dir[PATH_MAX], status[PATH_MAX + 32], pidtxt[32];
            family_dir(names[i], dir, sizeof dir);
            read_leaf(dir, "cmd", cmd, sizeof cmd);
            read_leaf(dir, "pid", pidtxt, sizeof pidtxt);
            read_leaf(dir, "exit", status, sizeof status);
            printf("%-24s %-8s %-18s %-9s %s\n", names[i], pidtxt[0] ? pidtxt : "-",
                   status[0] ? status : "gone (no exit record)", "-", cmd);
        }
        free(names[i]);
    }
    return FK_OK;
}

/* ---- tree ----------------------------------------------------------------- */

struct mem { pid_t pid, old_ppid; char origin; };

static void tree_print(const struct procent *ps, int n, pid_t pid, const char *prefix, const struct mem *ms,
                       int nm, int depth)
{
    if (depth > 64)
        return;
    int kids = 0;
    for (int i = 0; i < n; i++)
        if (ps[i].ppid == pid)
            kids++;
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (ps[i].ppid != pid)
            continue;
        bool last = ++seen == kids;
        char cmd[80], note[64] = "";
        if (proc_cmdline(ps[i].pid, cmd, sizeof cmd) < 0)
            snprintf(cmd, sizeof cmd, "?");
        for (int j = 0; j < nm; j++) {
            if (ms[j].pid != ps[i].pid)
                continue;
            if (ms[j].origin == 'a')
                snprintf(note, sizeof note, "  (adopted from %d)", ms[j].old_ppid);
            else if (ms[j].origin == 'o')
                snprintf(note, sizeof note, "  (orphan, adopted)");
        }
        printf("%s%s%d %s%s\n", prefix, last ? "└─ " : "├─ ", ps[i].pid, cmd, note);
        char next[512];
        snprintf(next, sizeof next, "%s%s", prefix, last ? "   " : "│  ");
        tree_print(ps, n, ps[i].pid, next, ms, nm, depth + 1);
    }
}

int cmd_tree(const char *name)
{
    pid_t sup;
    if (client_info(name, &sup, NULL, NULL, 0) < 0) {
        fprintf(stderr, "focker: no running family '%s'\n", name);
        return FK_FAIL;
    }
    struct sbuf r = { 0 };
    char *args[] = { "members" };
    struct mem ms[MAX_MEMBERS];
    int nm = 0;
    if (client_request(name, args, 1, &r) == 0 && r.p) {
        for (char *line = r.p; line && *line && nm < MAX_MEMBERS;) {
            int pid, old, alive;
            char origin;
            if (sscanf(line, "%d %c %d %d", &pid, &origin, &old, &alive) == 4)
                ms[nm++] = (struct mem){ pid, old, origin };
            char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
        }
    }
    sb_free(&r);
    struct procent *ps;
    int n = proc_list(&ps);
    if (n < 0) {
        fprintf(stderr, "focker: cannot read /proc: %s\n", strerror(errno));
        return FK_FAIL;
    }
    printf("%d focker[%s]\n", sup, name);
    tree_print(ps, n, sup, "", ms, nm, 0);
    free(ps);
    return FK_OK;
}

/* ---- logs, rm, freeze/thaw -------------------------------------------------- */

static int cat_file(const char *path, bool follow, const char *name)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "focker: %s: %s\n", path, strerror(errno));
        return FK_FAIL;
    }
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
                break;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (!follow)
            break;
        fflush(stdout);
        if (client_info(name, NULL, NULL, NULL, 0) < 0) { /* family gone: print the tail and stop */
            while ((n = read(fd, buf, sizeof buf)) > 0)
                fwrite(buf, 1, (size_t)n, stdout);
            break;
        }
        sleep_ms(200);
    }
    close(fd);
    return FK_OK;
}

int cmd_logs(const char *name, pid_t adopted, bool follow)
{
    char dir[PATH_MAX];
    if (family_dir(name, dir, sizeof dir) < 0 || access(dir, F_OK) < 0) {
        fprintf(stderr, "focker: no family '%s'\n", name);
        return FK_FAIL;
    }
    char path[PATH_MAX + 64];
    if (!adopted) {
        snprintf(path, sizeof path, "%s/focker.log", dir);
        if (access(path, F_OK) < 0) {
            fprintf(stderr, "focker: '%s' has no log (it runs in the foreground; its output is on its terminal)\n",
                    name);
            return FK_FAIL;
        }
        return cat_file(path, follow, name);
    }
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "focker: %s: %s\n", dir, strerror(errno));
        return FK_FAIL;
    }
    char want[64];
    snprintf(want, sizeof want, "adopted-%d-fd", adopted);
    char *files[64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 64)
        if (strncmp(e->d_name, want, strlen(want)) == 0)
            files[n++] = strdup(e->d_name);
    closedir(d);
    if (n == 0) {
        fprintf(stderr, "focker: no captured output for %d in '%s'\n", adopted, name);
        return FK_FAIL;
    }
    qsort(files, (size_t)n, sizeof *files, cmp_str);
    int rc = FK_OK;
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        if (n > 1)
            printf("==> %s <==\n", files[i]);
        if (cat_file(path, follow && n == 1, name) != FK_OK)
            rc = FK_FAIL;
        free(files[i]);
    }
    return rc;
}

int cmd_rm(const char *name, bool quiet)
{
    char dir[PATH_MAX];
    if (family_dir(name, dir, sizeof dir) < 0 || access(dir, F_OK) < 0) {
        fprintf(stderr, "focker: no family '%s'\n", name);
        return FK_FAIL;
    }
    if (client_info(name, NULL, NULL, NULL, 0) == 0) {
        fprintf(stderr, "focker: '%s' is still running; 'focker stop %s' first\n", name, name);
        return FK_FAIL;
    }
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "focker: %s: %s\n", dir, strerror(errno));
        return FK_FAIL;
    }
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            unlinkat(dirfd(d), e->d_name, 0);
    closedir(d);
    if (rmdir(dir) < 0) {
        fprintf(stderr, "focker: cannot remove %s: %s\n", dir, strerror(errno));
        return FK_FAIL;
    }
    if (!quiet)
        printf("%s\n", name);
    return FK_OK;
}

int cmd_signal_pids(const char *name, const pid_t *pids, int n, bool stop)
{
    pid_t sup;
    if (client_info(name, &sup, NULL, NULL, 0) < 0) {
        fprintf(stderr, "focker: no running family '%s'\n", name);
        return FK_FAIL;
    }
    int rc = FK_OK;
    for (int i = 0; i < n; i++) {
        if (!proc_is_descendant(pids[i], sup)) {
            fprintf(stderr, "focker: %d is not in family '%s'\n", pids[i], name);
            rc = FK_FAIL;
            continue;
        }
        if (kill(pids[i], stop ? SIGSTOP : SIGCONT) < 0) {
            fprintf(stderr, "focker: %d: %s\n", pids[i], strerror(errno));
            rc = FK_FAIL;
            continue;
        }
        if (stop) {
            long deadline = now_ms() + 2000;
            char s;
            while ((s = proc_state(pids[i])) != 'T' && s != 't' && s && now_ms() < deadline)
                sleep_ms(5);
            if (s != 'T' && s != 't') {
                fprintf(stderr, "focker: %d did not stop\n", pids[i]);
                rc = FK_FAIL;
                continue;
            }
        }
        printf("%s %d\n", stop ? "froze" : "thawed", pids[i]);
    }
    return rc;
}
