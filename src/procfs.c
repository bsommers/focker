/* Small, allocation-light readers for /proc. Every function tolerates the
 * process vanishing underneath it and reports that as an error. */
#include "focker.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *proc_root = "/proc";

static ssize_t read_small(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off + 1 < len) {
        ssize_t n = read(fd, buf + off, len - 1 - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            errno = e;
            return -1;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    buf[off] = '\0';
    return (ssize_t)off;
}

static int status_field(const char *path, const char *key, char *out, size_t outlen)
{
    char buf[8192];
    if (read_small(path, buf, sizeof buf) < 0)
        return -1;
    size_t klen = strlen(key);
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (strncmp(line, key, klen) == 0) {
            char *v = line + klen;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t n = nl ? (size_t)(nl - v) : strlen(v);
            if (n >= outlen)
                n = outlen - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
        line = nl ? nl + 1 : NULL;
    }
    errno = ENOENT;
    return -1;
}

static bool numeric(const char *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s))
            return false;
    return true;
}

static int cmp_pid(const void *a, const void *b)
{
    pid_t x = *(const pid_t *)a, y = *(const pid_t *)b;
    return (x > y) - (x < y);
}

pid_t proc_ppid(pid_t pid)
{
    char path[PATH_MAX], v[32];
    snprintf(path, sizeof path, "%s/%d/status", proc_root, pid);
    if (status_field(path, "PPid:", v, sizeof v) < 0)
        return -1;
    return (pid_t)atoi(v);
}

char proc_state(pid_t pid)
{
    char path[PATH_MAX], v[32];
    snprintf(path, sizeof path, "%s/%d/status", proc_root, pid);
    if (status_field(path, "State:", v, sizeof v) < 0)
        return 0;
    return v[0];
}

int proc_cmdline(pid_t pid, char *buf, size_t len)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%d/cmdline", proc_root, pid);
    ssize_t n = read_small(path, buf, len);
    if (n < 0)
        return -1;
    while (n > 0 && buf[n - 1] == '\0')
        n--;
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\0')
            buf[i] = ' ';
    buf[n] = '\0';
    if (n == 0) { /* kernel thread or zombie: fall back to comm */
        char comm[64];
        snprintf(path, sizeof path, "%s/%d/comm", proc_root, pid);
        ssize_t c = read_small(path, comm, sizeof comm);
        if (c < 0)
            return -1;
        if (c > 0 && comm[c - 1] == '\n')
            comm[c - 1] = '\0';
        snprintf(buf, len, "[%s]", comm);
    }
    return 0;
}

int proc_children(pid_t parent, pid_t *out, int max)
{
    DIR *d = opendir(proc_root);
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!numeric(e->d_name))
            continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        if (proc_ppid(pid) == parent)
            out[n++] = pid;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_pid);
    return n;
}

int proc_list(struct procent **out)
{
    DIR *d = opendir(proc_root);
    if (!d)
        return -1;
    struct procent *v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!numeric(e->d_name))
            continue;
        pid_t pid = (pid_t)atoi(e->d_name);
        pid_t ppid = proc_ppid(pid);
        if (ppid < 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            struct procent *nv = realloc(v, (size_t)cap * sizeof *v);
            if (!nv) {
                free(v);
                closedir(d);
                return -1;
            }
            v = nv;
        }
        v[n].pid = pid;
        v[n].ppid = ppid;
        n++;
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof *v, cmp_pid); /* pid is the first member */
    *out = v;
    return n;
}

bool proc_is_descendant(pid_t pid, pid_t ancestor)
{
    for (int depth = 0; depth < 4096 && pid > 1; depth++) {
        pid_t pp = proc_ppid(pid);
        if (pp <= 0)
            return false;
        if (pp == ancestor)
            return true;
        pid = pp;
    }
    return false;
}

int proc_threads(pid_t pid, pid_t *out, int max)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%d/task", proc_root, pid);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max)
        if (numeric(e->d_name))
            out[n++] = (pid_t)atoi(e->d_name);
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_pid);
    return n;
}

static int cmp_fd(const void *a, const void *b)
{
    const struct fdent *x = a, *y = b;
    return (x->fd > y->fd) - (x->fd < y->fd);
}

int proc_fds(pid_t pid, struct fdent *out, int max)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%d/fd", proc_root, pid);
    DIR *d = opendir(path);
    if (!d)
        return -1;
    int dfd = dirfd(d), n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!numeric(e->d_name))
            continue;
        char link[128];
        ssize_t l = readlinkat(dfd, e->d_name, link, sizeof link - 1);
        if (l < 0)
            continue;
        link[l] = '\0';
        struct fdent *f = &out[n];
        f->fd = atoi(e->d_name);
        f->ino = 0;
        f->kind = 'o';
        if (sscanf(link, "pipe:[%lu]", &f->ino) == 1)
            f->kind = 'p';
        else if (sscanf(link, "socket:[%lu]", &f->ino) == 1)
            f->kind = 's';
        char ipath[PATH_MAX], flags[32];
        snprintf(ipath, sizeof ipath, "%s/%d/fdinfo/%d", proc_root, pid, f->fd);
        f->accmode = status_field(ipath, "flags:", flags, sizeof flags) == 0
                         ? (int)(strtol(flags, NULL, 8) & O_ACCMODE)
                         : O_RDWR;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmp_fd);
    return n;
}

uint64_t proc_sigpending(pid_t pid)
{
    uint64_t mask = 0;
    char path[PATH_MAX], v[64];
    snprintf(path, sizeof path, "%s/%d/status", proc_root, pid);
    if (status_field(path, "ShdPnd:", v, sizeof v) == 0)
        mask |= strtoull(v, NULL, 16);
    pid_t tids[MAX_TIDS];
    int n = proc_threads(pid, tids, MAX_TIDS);
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/%d/task/%d/status", proc_root, pid, tids[i]);
        if (status_field(path, "SigPnd:", v, sizeof v) == 0)
            mask |= strtoull(v, NULL, 16);
    }
    return mask;
}
