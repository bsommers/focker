#include "focker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *log_prefix = "focker";

void sb_printf(struct sbuf *sb, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        size_t avail = sb->cap - sb->len;
        va_start(ap, fmt);
        int n = vsnprintf(sb->p ? sb->p + sb->len : NULL, avail, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if ((size_t)n < avail) {
            sb->len += (size_t)n;
            return;
        }
        size_t ncap = sb->cap ? sb->cap * 2 : 256;
        while (ncap - sb->len <= (size_t)n)
            ncap *= 2;
        char *np = realloc(sb->p, ncap);
        if (!np)
            return;
        sb->p = np;
        sb->cap = ncap;
    }
}

/* Length of the valid UTF-8 sequence at s, or 0 if it isn't one (bad lead
 * byte, missing continuation, overlong form, surrogate, or past U+10FFFF). */
static int utf8_len(const unsigned char *s)
{
    unsigned char c = s[0];
    int n;
    unsigned char lo = 0x80, hi = 0xbf; /* allowed range of the second byte */
    if (c >= 0xc2 && c <= 0xdf)
        n = 2;
    else if (c >= 0xe0 && c <= 0xef) {
        n = 3;
        if (c == 0xe0)
            lo = 0xa0; /* overlong */
        if (c == 0xed)
            hi = 0x9f; /* surrogates */
    } else if (c >= 0xf0 && c <= 0xf4) {
        n = 4;
        if (c == 0xf0)
            lo = 0x90;
        if (c == 0xf4)
            hi = 0x8f;
    } else
        return 0;
    if (s[1] < lo || s[1] > hi)
        return 0;
    for (int i = 2; i < n; i++)
        if (s[i] < 0x80 || s[i] > 0xbf)
            return 0;
    return n;
}

void sb_json_str(struct sbuf *sb, const char *s)
{
    if (!s) {
        sb_printf(sb, "null");
        return;
    }
    sb_printf(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        if (*p >= 0x80) {
            int n = utf8_len(p);
            if (n) {
                sb_printf(sb, "%.*s", n, (const char *)p);
                p += n;
            } else {
                sb_printf(sb, "\\ufffd");
                p++;
            }
            continue;
        }
        if (*p == '"' || *p == '\\')
            sb_printf(sb, "\\%c", *p);
        else if (*p == '\n')
            sb_printf(sb, "\\n");
        else if (*p == '\t')
            sb_printf(sb, "\\t");
        else if (*p < 0x20 || *p == 0x7f)
            sb_printf(sb, "\\u%04x", *p);
        else
            sb_printf(sb, "%c", *p);
        p++;
    }
    sb_printf(sb, "\"");
}

void sb_free(struct sbuf *sb)
{
    free(sb->p);
    sb->p = NULL;
    sb->len = sb->cap = 0;
}

void logmsg(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s: %s\n", log_prefix, buf);
}

/* FOCKER_RUNTIME_DIR > $XDG_RUNTIME_DIR/focker > /tmp/focker-$UID */
int runtime_dir(char *buf, size_t len)
{
    const char *env = getenv("FOCKER_RUNTIME_DIR");
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    int n;
    if (env && *env)
        n = snprintf(buf, len, "%s", env);
    else if (xdg && *xdg)
        n = snprintf(buf, len, "%s/focker", xdg);
    else
        n = snprintf(buf, len, "/tmp/focker-%d", (int)getuid());
    if (n < 0 || (size_t)n >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(buf, 0700) < 0 && errno != EEXIST)
        return -1;
    struct stat st;
    if (lstat(buf, &st) < 0)
        return -1;
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        errno = EPERM; /* someone else's directory or a symlink: refuse */
        return -1;
    }
    return 0;
}

int family_dir(const char *name, char *buf, size_t len)
{
    char rt[PATH_MAX];
    if (runtime_dir(rt, sizeof rt) < 0)
        return -1;
    int n = snprintf(buf, len, "%s/%s", rt, name);
    if (n < 0 || (size_t)n >= len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* sun_path holds only 108 bytes. When the state dir is deeper than that, go
 * through /proc/self/fd/N, which resolves to the directory and stays short. */
int control_addr(const char *dir, struct sockaddr_un *a, int *dirfd)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    *dirfd = -1;
    if (snprintf(a->sun_path, sizeof a->sun_path, "%s/control.sock", dir) < (int)sizeof a->sun_path)
        return 0;
    *dirfd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (*dirfd < 0)
        return -1;
    snprintf(a->sun_path, sizeof a->sun_path, "/proc/self/fd/%d/control.sock", *dirfd);
    return 0;
}

bool valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n > NAME_MAX_LEN || name[0] == '.' || name[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

int parse_signal(const char *s)
{
    if (!s || !*s)
        return -1;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end == '\0')
        return (v >= 1 && v < NSIG) ? (int)v : -1;
    if (strncasecmp(s, "SIG", 3) == 0)
        s += 3;
    for (int i = 1; i < NSIG; i++) {
        const char *abbr = sigabbrev_np(i);
        if (abbr && strcasecmp(abbr, s) == 0)
            return i;
    }
    return -1;
}

const char *signame(int sig)
{
    static char buf[4][32];
    static int slot;
    char *b = buf[slot++ % 4];
    const char *abbr = sigabbrev_np(sig);
    if (abbr)
        snprintf(b, sizeof buf[0], "SIG%s", abbr);
    else
        snprintf(b, sizeof buf[0], "signal %d", sig);
    return b;
}

const char *describe_status(int status)
{
    static char buf[64];
    if (WIFEXITED(status))
        snprintf(buf, sizeof buf, "exited %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(buf, sizeof buf, "killed by %s", signame(WTERMSIG(status)));
    else
        snprintf(buf, sizeof buf, "status 0x%x", status);
    return buf;
}

long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

int pidfd_open_(pid_t pid)
{
    return (int)syscall(SYS_pidfd_open, pid, 0);
}

int pidfd_getfd_(int pidfd, int fd)
{
    return (int)syscall(SYS_pidfd_getfd, pidfd, fd, 0);
}

int pidfd_signal_(int pidfd, int sig)
{
    return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
}
