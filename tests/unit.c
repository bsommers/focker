/* Unit tests for the /proc readers (against a fixture tree) and the unix
 * socket peer lookup (against a live socketpair). Run with: make unit */
#include "../src/focker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int pass, fail;
static char root[PATH_MAX / 2]; /* half, so root + a relative path always fits */

#define CHECK(name, cond)                                                       \
    do {                                                                        \
        if (cond) {                                                             \
            pass++;                                                             \
            printf("  ok    %s\n", name);                                       \
        } else {                                                                \
            fail++;                                                             \
            printf("  FAIL  %s (%s:%d: %s)\n", name, __FILE__, __LINE__, #cond); \
        }                                                                       \
    } while (0)

/* write root/REL (creating parent dirs), with an explicit length so NULs survive */
static void put(const char *rel, const char *data, size_t len)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    for (char *p = path + strlen(root) + 1; (p = strchr(p, '/')); p++) {
        *p = '\0';
        mkdir(path, 0700);
        *p = '/';
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}
#define PUT(rel, s) put(rel, s, sizeof s - 1)

static void link_fd(const char *rel, const char *target)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    if (symlink(target, path) < 0) {
        perror(path);
        exit(1);
    }
}

static void build_fixture(void)
{
    snprintf(root, sizeof root, "%s/focker-unit.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        exit(1);
    }
    PUT("100/status", "Name:\tparent\nState:\tS (sleeping)\nTracerPid:\t0\nPPid:\t1\nShdPnd:\t0000000000000001\n");
    PUT("100/cmdline", "prog\0-x\0two words\0");
    PUT("100/task/100/status", "Name:\tparent\nSigPnd:\t0000000000000200\n");
    PUT("100/task/101/status", "Name:\tworker\nSigPnd:\t0000000000004000\n");
    /* children forked by two threads; 102 is listed twice, as a reparent between reads can do */
    PUT("100/task/100/children", "102 101 ");
    PUT("100/task/101/children", "103 102 ");
    PUT("101/status", "Name:\tzomb\nState:\tZ (zombie)\nPPid:\t100\n");
    PUT("101/cmdline", "");
    PUT("101/comm", "zomb\n");
    PUT("103/status", "Name:\tc\nState:\tT (stopped)\nPPid:\t100\n");
    PUT("102/status", "Name:\tb\nState:\tR (running)\nPPid:\t100\n");
    PUT("104/status", "Name:\tgrandchild\nState:\tS\nPPid:\t103\n");
    PUT("self/status", "PPid:\t100\n"); /* non-numeric entries are ignored */
    PUT("12x/status", "PPid:\t100\n");

    PUT("100/fd/.keep", ""); /* creates the fd dir for the links below */
    link_fd("100/fd/10", "pipe:[4242]");
    link_fd("100/fd/3", "pipe:[4242]");
    link_fd("100/fd/4", "socket:[77]");
    link_fd("100/fd/5", "/dev/null");
    link_fd("100/fd/6", "pipe:[9]");
    PUT("100/fdinfo/10", "pos:\t0\nflags:\t02000000\nmnt_id:\t15\n");
    PUT("100/fdinfo/3", "pos:\t0\nflags:\t02000001\nmnt_id:\t15\n");
    PUT("100/fdinfo/4", "pos:\t0\nflags:\t02000002\n");
    PUT("100/fdinfo/5", "pos:\t0\nflags:\t0100000\n");
    /* no fdinfo for fd 6: treated as O_RDWR */
}

static void test_status(void)
{
    puts("status fields");
    CHECK("ppid", proc_ppid(100) == 1);
    CHECK("ppid ignores TracerPid", proc_ppid(102) == 100);
    CHECK("ppid of a missing pid is -1", proc_ppid(999) == -1);
    CHECK("state letter", proc_state(103) == 'T' && proc_state(101) == 'Z');
    CHECK("state of a missing pid is 0", proc_state(999) == 0);
}

static void test_cmdline(void)
{
    char buf[64];
    puts("cmdline");
    CHECK("NULs become spaces", proc_cmdline(100, buf, sizeof buf) == 0 && strcmp(buf, "prog -x two words") == 0);
    CHECK("empty cmdline falls back to [comm]", proc_cmdline(101, buf, sizeof buf) == 0 && strcmp(buf, "[zomb]") == 0);
    CHECK("truncates to the buffer", proc_cmdline(100, buf, 5) == 0 && strcmp(buf, "prog") == 0);
    CHECK("missing pid fails", proc_cmdline(999, buf, sizeof buf) < 0);
}

static void test_tree(void)
{
    pid_t v[8];
    puts("children and descendants");
    int n = proc_children(100, v, 8);
    CHECK("children are found and sorted", n == 3 && v[0] == 101 && v[1] == 102 && v[2] == 103);
    CHECK("non-numeric entries are skipped", n == 3);
    CHECK("children stop at max", proc_children(100, v, 2) == 2);
    CHECK("no children", proc_children(104, v, 8) == 0);
    struct procent *all;
    n = proc_list(&all);
    CHECK("proc_list sees every numeric entry, sorted", n == 5 && all[0].pid == 100 && all[4].pid == 104 &&
                                                            all[4].ppid == 103);
    free(all);
    CHECK("grandchild is a descendant", proc_is_descendant(104, 100));
    CHECK("parent is not a descendant of its child", !proc_is_descendant(100, 104));
    CHECK("missing pid is not a descendant", !proc_is_descendant(999, 100));
    n = proc_threads(100, v, 8);
    CHECK("threads are listed and sorted", n == 2 && v[0] == 100 && v[1] == 101);
    CHECK("sigpending ORs ShdPnd with every thread's SigPnd",
          proc_sigpending(100) == ((1ULL << (SIGHUP - 1)) | (1ULL << (SIGUSR1 - 1)) | (1ULL << (SIGTERM - 1))));
    CHECK("sigpending of a missing pid is empty", proc_sigpending(999) == 0);
}

static void children_both(pid_t pid, int max, pid_t *fast, int *nfast, pid_t *scan, int *nscan)
{
    proc_children_scan_only = false;
    *nfast = proc_children(pid, fast, max);
    proc_children_scan_only = true;
    *nscan = proc_children(pid, scan, max);
    proc_children_scan_only = false;
}

static void test_children_both_ways(void)
{
    pid_t f[8], s[8];
    int nf, ns;
    puts("children: task/*/children vs full scan");
    children_both(100, 8, f, &nf, s, &ns);
    CHECK("fast path merges every thread's list, sorted and deduped",
          nf == 3 && f[0] == 101 && f[1] == 102 && f[2] == 103);
    CHECK("fast path matches the scan", nf == ns && memcmp(f, s, sizeof f[0] * (size_t)nf) == 0);
    PUT("100/task/101/children", "");
    PUT("100/task/100/children", "101 ");
    children_both(100, 8, f, &nf, s, &ns);
    CHECK("the fast path is really used", nf == 1 && f[0] == 101 && ns == 3);
    PUT("100/task/100/children", "102 101 ");
    PUT("100/task/101/children", "103 102 ");
    children_both(103, 8, f, &nf, s, &ns);
    CHECK("no task/*/children falls back to the scan", nf == 1 && f[0] == 104 && ns == 1);
    children_both(100, 2, f, &nf, s, &ns);
    CHECK("both modes cap at max with the lowest pids", nf == 2 && f[0] == 101 && f[1] == 102 && ns == 2 &&
                                                            s[0] == 101 && s[1] == 102);
    children_both(999, 8, f, &nf, s, &ns);
    CHECK("a missing parent has no children either way", nf == 0 && ns == 0);

    proc_root = "/proc";
    pid_t kids[3];
    for (int i = 0; i < 3; i++)
        if ((kids[i] = fork()) == 0) {
            pause();
            _exit(0);
        }
    pid_t lf[64], ls[64];
    children_both(getpid(), 64, lf, &nf, ls, &ns);
    int found = 0;
    for (int i = 0; i < nf; i++)
        for (int k = 0; k < 3; k++)
            found += lf[i] == kids[k];
    CHECK("live: the fast path finds our children", nf == 3 && found == 3);
    CHECK("live: fast path matches the scan", nf == ns && memcmp(lf, ls, sizeof lf[0] * (size_t)nf) == 0);
    for (int i = 0; i < 3; i++) {
        kill(kids[i], SIGKILL);
        waitpid(kids[i], NULL, 0);
    }
    proc_root = root;
}

static void test_fds(void)
{
    struct fdent fe[8];
    puts("fds");
    int n = proc_fds(100, fe, 8);
    CHECK("all fds are listed", n == 5);
    CHECK("sorted numerically, not as strings", n == 5 && fe[0].fd == 3 && fe[4].fd == 10);
    CHECK("pipe write end", fe[0].kind == 'p' && fe[0].ino == 4242 && fe[0].accmode == O_WRONLY);
    CHECK("socket", fe[1].kind == 's' && fe[1].ino == 77 && fe[1].accmode == O_RDWR);
    CHECK("other files", fe[2].kind == 'o' && fe[2].ino == 0 && fe[2].accmode == O_RDONLY);
    CHECK("missing fdinfo means O_RDWR", fe[3].kind == 'p' && fe[3].accmode == O_RDWR);
    CHECK("pipe read end", fe[4].kind == 'p' && fe[4].ino == 4242 && fe[4].accmode == O_RDONLY);
    CHECK("fds stop at max", proc_fds(100, fe, 2) == 2);
    CHECK("missing fd dir fails", proc_fds(102, fe, 8) < 0);
}

static bool json_is(const char *in, const char *want)
{
    struct sbuf b = { 0 };
    sb_json_str(&b, in);
    bool ok = b.p && strcmp(b.p, want) == 0;
    if (!ok)
        printf("        got %s, wanted %s\n", b.p ? b.p : "(nothing)", want);
    sb_free(&b);
    return ok;
}

static void test_json_str(void)
{
    puts("json strings");
    CHECK("plain", json_is("abc", "\"abc\""));
    CHECK("quotes and backslash", json_is("a\"b\\c", "\"a\\\"b\\\\c\""));
    CHECK("control chars", json_is("\n\t\x01", "\"\\n\\t\\u0001\""));
    CHECK("utf-8 passes through", json_is("h\xc3\xa9", "\"h\xc3\xa9\""));
    CHECK("4-byte utf-8 passes through", json_is("\xf0\x9f\x98\x80", "\"\xf0\x9f\x98\x80\""));
    CHECK("invalid utf-8 is replaced", json_is("a\xff" "b", "\"a\\ufffdb\""));
    CHECK("a truncated sequence is replaced", json_is("\xc3", "\"\\ufffd\""));
    CHECK("an overlong encoding is replaced", json_is("\xc0\xaf", "\"\\ufffd\\ufffd\""));
    CHECK("a surrogate is replaced", json_is("\xed\xa0\x80", "\"\\ufffd\\ufffd\\ufffd\""));
    CHECK("NULL is null", json_is(NULL, "null"));
}

static unsigned long ino_of(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (unsigned long)st.st_ino : 0;
}

static void test_live(void)
{
    puts("live /proc and sock_diag");
    proc_root = "/proc";
    int p[2], s[2];
    if (pipe(p) < 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, s) < 0) {
        perror("pipe/socketpair");
        exit(1);
    }
    static struct fdent fe[MAX_FDENTS];
    int n = proc_fds(getpid(), fe, MAX_FDENTS);
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (fe[i].fd == p[0] && fe[i].kind == 'p' && fe[i].accmode == O_RDONLY)
            seen |= 1;
        if (fe[i].fd == p[1] && fe[i].kind == 'p' && fe[i].accmode == O_WRONLY)
            seen |= 2;
        if (fe[i].fd == s[0] && fe[i].kind == 's' && fe[i].ino == ino_of(s[0]))
            seen |= 4;
    }
    CHECK("a real pipe's ends and a socket are classified", seen == 7);
    CHECK("our own ppid", proc_ppid(getpid()) == getppid());
    CHECK("unixdiag dump succeeds", unixdiag_load() == 0);
    CHECK("socketpair peer, one way", unixdiag_peer(ino_of(s[0])) == ino_of(s[1]));
    CHECK("socketpair peer, the other way", unixdiag_peer(ino_of(s[1])) == ino_of(s[0]));
    CHECK("unknown inode has no peer", unixdiag_peer(1) == 0);
    close(p[0]);
    close(p[1]);
    close(s[0]);
    close(s[1]);
}

static bool loop_is(int family, const char *text)
{
    unsigned char a[16];
    return inet_pton(family, text, a) == 1 && inet_is_loopback(family, a);
}

static void test_loopback(void)
{
    puts("loopback classifier");
    CHECK("127.0.0.1", loop_is(AF_INET, "127.0.0.1"));
    CHECK("127.255.0.9", loop_is(AF_INET, "127.255.0.9"));
    CHECK("::1", loop_is(AF_INET6, "::1"));
    CHECK("::ffff:127.0.0.1", loop_is(AF_INET6, "::ffff:127.0.0.1"));
    CHECK("not 10.0.0.1", !loop_is(AF_INET, "10.0.0.1"));
    CHECK("not 192.168.1.5", !loop_is(AF_INET, "192.168.1.5"));
    CHECK("not ::", !loop_is(AF_INET6, "::"));
    CHECK("not ::ffff:10.0.0.1", !loop_is(AF_INET6, "::ffff:10.0.0.1"));
    CHECK("not fe80::1", !loop_is(AF_INET6, "fe80::1"));
}

/* Listen on loopback, connect twice, accept both. Returns -1 if the family
 * can't bind loopback (no IPv6), leaving every fd at -1. */
static int tcp_pairs(int family, int *lis, int c[2], int s[2])
{
    struct sockaddr_storage ss = { 0 };
    socklen_t len;
    int l, cc[2] = { -1, -1 }, sc[2] = { -1, -1 };
    *lis = c[0] = c[1] = s[0] = s[1] = -1;
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof *a;
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_loopback;
        len = sizeof *a;
    }
    l = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (l < 0)
        return -1;
    if (bind(l, (struct sockaddr *)&ss, len) < 0 || listen(l, 4) < 0 ||
        getsockname(l, (struct sockaddr *)&ss, &len) < 0)
        goto fail;
    for (int i = 0; i < 2; i++) {
        cc[i] = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (cc[i] < 0 || connect(cc[i], (struct sockaddr *)&ss, len) < 0)
            goto fail;
        sc[i] = accept4(l, NULL, NULL, SOCK_CLOEXEC);
        if (sc[i] < 0)
            goto fail;
    }
    *lis = l;
    for (int i = 0; i < 2; i++) {
        c[i] = cc[i];
        s[i] = sc[i];
    }
    return 0;
fail:
    for (int i = 0; i < 2; i++) {
        if (cc[i] >= 0)
            close(cc[i]);
        if (sc[i] >= 0)
            close(sc[i]);
    }
    close(l);
    return -1;
}

static void test_tcp_family(int family, const char *tag, const char *prefix, const char *mid)
{
    int lis, c[2], s[2];
    char name[96], ends[128];
    if (tcp_pairs(family, &lis, c, s) < 0) {
        printf("  skip  live %s tcp pairs (cannot bind loopback)\n", tag);
        return;
    }
#define TCHECK(what, cond)                                     \
    do {                                                       \
        snprintf(name, sizeof name, "%s: %s", tag, what);      \
        CHECK(name, cond);                                     \
    } while (0)
    TCHECK("inetdiag dump succeeds", inetdiag_load() == 0);
    TCHECK("first client pairs with its own server end", inetdiag_peer(ino_of(c[0])) == ino_of(s[0]));
    TCHECK("and back", inetdiag_peer(ino_of(s[0])) == ino_of(c[0]));
    TCHECK("second client pairs with its own server end", inetdiag_peer(ino_of(c[1])) == ino_of(s[1]));
    TCHECK("and back", inetdiag_peer(ino_of(s[1])) == ino_of(c[1]));
    TCHECK("a listening socket has no peer", inetdiag_peer(ino_of(lis)) == 0);
    TCHECK("an unknown inode has no peer", inetdiag_peer(1) == 0);
    TCHECK("endpoints are formatted",
           inetdiag_endpoints(ino_of(c[0]), ends, sizeof ends) == 0 &&
               strncmp(ends, prefix, strlen(prefix)) == 0 && strstr(ends, mid) != NULL);
    TCHECK("endpoints of an unknown inode fail", inetdiag_endpoints(1, ends, sizeof ends) < 0);
#undef TCHECK
    for (int i = 0; i < 2; i++) {
        close(c[i]);
        close(s[i]);
    }
    close(lis);
}

/* A dual-stack IPv6 listener on ::ffff:127.0.0.1 accepts an IPv4 client: the
 * accepted socket reports v4-mapped addresses and must still pair. */
static void test_tcp_mapped(void)
{
    struct sockaddr_in6 a6 = { .sin6_family = AF_INET6 };
    socklen_t len = sizeof a6;
    int off = 0, l = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0), c = -1, s = -1;
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &a6.sin6_addr);
    if (l < 0 || setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off) < 0 ||
        bind(l, (struct sockaddr *)&a6, sizeof a6) < 0 || listen(l, 1) < 0 ||
        getsockname(l, (struct sockaddr *)&a6, &len) < 0) {
        printf("  skip  mapped: cannot bind ::ffff:127.0.0.1\n");
        if (l >= 0)
            close(l);
        return;
    }
    struct sockaddr_in a4 = { .sin_family = AF_INET, .sin_port = a6.sin6_port };
    a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    c = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c >= 0 && connect(c, (struct sockaddr *)&a4, sizeof a4) == 0)
        s = accept4(l, NULL, NULL, SOCK_CLOEXEC);
    CHECK("mapped: connection set up", s >= 0);
    CHECK("mapped: inetdiag dump succeeds", inetdiag_load() == 0);
    CHECK("mapped: v4 client pairs with the dual-stack server end", s >= 0 && inetdiag_peer(ino_of(c)) == ino_of(s));
    CHECK("mapped: and back", s >= 0 && inetdiag_peer(ino_of(s)) == ino_of(c));
    if (s >= 0)
        close(s);
    if (c >= 0)
        close(c);
    close(l);
}

static void test_tcp(void)
{
    puts("live tcp pairs");
    test_tcp_family(AF_INET, "v4", "127.0.0.1:", " <-> 127.0.0.1:");
    test_tcp_family(AF_INET6, "v6", "[::1]:", " <-> [::1]:");
    test_tcp_mapped();
}

static int roots_of(const struct procent *ps, const pid_t *pgids, int n, pid_t pgid, pid_t *roots)
{
    return job_roots(ps, n, pgids, pgid, roots, MAX_ROOTS);
}

static void test_job_roots(void)
{
    puts("job roots");
    pid_t r[MAX_ROOTS];
    /* pid, ppid (sorted by pid, as proc_list returns them) and each one's pgid */
    const struct procent ps[] = { { 1, 0 },   { 50, 1 },  { 60, 50 }, { 61, 50 }, { 62, 50 }, { 63, 50 },
                                  { 70, 50 }, { 71, 70 }, { 72, 71 }, { 80, 50 }, { 85, 95 }, { 90, 50 },
                                  { 95, 50 } };
    const pid_t pg[] = { 1, 50, 60, 61, 61, 61, 70, 70, 70, 90, 95, 90, 95 };
    int n = (int)(sizeof ps / sizeof ps[0]);
    CHECK("a single process is its own root", roots_of(ps, pg, n, 60, r) == 1 && r[0] == 60);
    CHECK("a pipeline's siblings are each a root",
          roots_of(ps, pg, n, 61, r) == 3 && r[0] == 61 && r[1] == 62 && r[2] == 63);
    CHECK("children in the group are not roots", roots_of(ps, pg, n, 70, r) == 1 && r[0] == 70);
    CHECK("roots come back sorted by pid", roots_of(ps, pg, n, 90, r) == 2 && r[0] == 80 && r[1] == 90);
    CHECK("a child with a lower pid than its parent", roots_of(ps, pg, n, 95, r) == 1 && r[0] == 95);
    CHECK("an empty group has no roots", roots_of(ps, pg, n, 4242, r) == 0);
    CHECK("too many roots fails", job_roots(ps, n, pg, 61, r, 2) < 0);
}

static void join(char **argv, int argc, char *out, size_t len)
{
    out[0] = '\0';
    for (int i = 0; i < argc; i++)
        snprintf(out + strlen(out), len - strlen(out), "%s%s", i ? " " : "", argv[i]);
}

/* pipes-data.img: magic, then (u32 LE length, PipeDataEntry, raw bytes) per pipe */
static void test_pipe_data(void)
{
    static const unsigned char img[] = {
        1, 2, 3, 4, 5, 6, 7, 8,                                           /* magic */
        7, 0, 0, 0, 0x08, 77, 0x10, 3, 0x18, 0x80, 0x20, 'a', 'b', 'c',   /* pipe 77: 3 bytes, size 4096 */
        9, 0, 0, 0, 0x08, 0xac, 0x02, 0x10, 2, 0x18, 0x80, 0x80, 0x04, 'x', 'y', /* pipe 300: 2 bytes, 65536 */
    };
    size_t off, n;
    unsigned long size;
    CHECK("pipe data: first entry", criu_pipe_data(img, sizeof img, 77, &off, &n, &size) == 1 && n == 3 &&
                                        size == 4096 && memcmp(img + off, "abc", 3) == 0);
    CHECK("pipe data: second entry", criu_pipe_data(img, sizeof img, 300, &off, &n, &size) == 1 && n == 2 &&
                                         size == 65536 && memcmp(img + off, "xy", 2) == 0);
    CHECK("pipe data: absent pipe", criu_pipe_data(img, sizeof img, 5, &off, &n, &size) == 0);
    CHECK("pipe data: truncated bytes", criu_pipe_data(img, sizeof img - 1, 300, &off, &n, &size) == -1);
    CHECK("pipe data: truncated entry", criu_pipe_data(img, 14, 300, &off, &n, &size) == -1);
}

static void test_criu_argv(void)
{
    puts("criu argv");
    struct criu_job j = { .pgid = 61, .roots = { 61, 62 }, .nroots = 2, .pipes = { 4242, 77 }, .npipes = 2 };
    char *argv[64], got[1024];
    int argc = criu_dump_argv("/x/criu", &j, 62, "/run/img", true, argv, 64);
    join(argv, argc, got, sizeof got);
    CHECK("a trial dump leaves the job running",
          strcmp(got, "/x/criu dump --unprivileged --shell-job -t 62 -D /run/img -o dump.log --leave-running") == 0);
    CHECK("argv ends with NULL", argv[argc] == NULL);
    argc = criu_dump_argv("/x/criu", &j, 61, "/run/img", false, argv, 64);
    join(argv, argc, got, sizeof got);
    CHECK("the real dump leaves it stopped",
          strcmp(got, "/x/criu dump --unprivileged --shell-job -t 61 -D /run/img -o dump.log --leave-stopped") == 0);

    const int none[] = { -1, -1 }, one[] = { 5, -1 };
    argc = criu_restore_argv("/x/criu", &j, "/run/img", none, argv, 64);
    join(argv, argc, got, sizeof got);
    CHECK("restore without pipes",
          strcmp(got, "/x/criu restore --unprivileged --shell-job --restore-detached -D /run/img -o restore.log "
                      "--pidfile /run/img/restored.pid --leave-stopped") == 0);
    argc = criu_restore_argv("/x/criu", &j, "/run/img", one, argv, 64);
    join(argv, argc, got, sizeof got);
    CHECK("restore hands over the pipes this root holds",
          strcmp(got, "/x/criu restore --unprivileged --shell-job --restore-detached -D /run/img -o restore.log "
                      "--pidfile /run/img/restored.pid --leave-stopped --inherit-fd fd[5]:pipe:[4242]") == 0);
    CHECK("too small an argv fails", criu_dump_argv("/x/criu", &j, 61, "/run/img", true, argv, 4) < 0);
}

static void test_stat_fields(void)
{
    puts("stat fields");
    proc_root = root;
    PUT("300/stat", "300 (a (b) c) S 1 290 280 34817 290 4194304 0 0\n");
    PUT("300/status", "Name:\tx\nUid:\t1000\t1000\t1000\t1000\n");
    CHECK("pgid", proc_pgid(300) == 290);
    CHECK("sid", proc_sid(300) == 280);
    CHECK("tty_nr", proc_tty_nr(300) == 34817);
    CHECK("tpgid", proc_tpgid(300) == 290);
    CHECK("uid", proc_uid(300) == 1000);
    CHECK("a missing pid", proc_pgid(999) == -1 && proc_tty_nr(999) < 0 && proc_uid(999) == (uid_t)-1);
}

int main(void)
{
    build_fixture();
    proc_root = root;
    printf("focker unit tests (fixture %s)\n", root);
    test_json_str();
    test_status();
    test_cmdline();
    test_tree();
    test_children_both_ways();
    test_fds();
    test_live();
    test_loopback();
    test_tcp();
    test_stat_fields();
    test_job_roots();
    test_criu_argv();
    test_pipe_data();
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "unit: could not remove %s\n", root);
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
