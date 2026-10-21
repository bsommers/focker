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
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        fprintf(stderr, "unit: could not remove %s\n", root);
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
