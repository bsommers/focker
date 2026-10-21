/* focker: reparent running processes onto a new parent, cleanly. */
#ifndef FOCKER_H
#define FOCKER_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/un.h>

#define FOCKER_VERSION "1.0.0"

/* exit codes */
#define FK_OK    0
#define FK_FAIL  1
#define FK_USAGE 2

#define MAX_PIDS     64    /* processes in one cutover */
#define MAX_TIDS     256   /* threads per frozen process */
#define MAX_MEMBERS  256   /* processes a family remembers */
#define MAX_STOLEN   128   /* fds a family holds on behalf of adopted children */
#define MAX_FDENTS   1024  /* fds scanned per process */
#define MAX_ARGS     256   /* tokens in one control request */
#define NAME_MAX_LEN 48

/* util.c */
struct sbuf { char *p; size_t len, cap; };
void sb_printf(struct sbuf *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_free(struct sbuf *sb);
void sb_json_str(struct sbuf *sb, const char *s);     /* quoted JSON string, always valid UTF-8; NULL -> null */

extern const char *log_prefix;
void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  runtime_dir(char *buf, size_t len);              /* resolves and creates it */
int  family_dir(const char *name, char *buf, size_t len);
bool valid_name(const char *name);
int  control_addr(const char *dir, struct sockaddr_un *a, int *dirfd); /* close *dirfd after use */
int  parse_signal(const char *s);                     /* -1 on error */
const char *signame(int sig);
const char *describe_status(int status);              /* "exited 0", "killed by SIGTERM" */
long now_ms(void);
void sleep_ms(long ms);
int  pidfd_open_(pid_t pid);
int  pidfd_getfd_(int pidfd, int fd);
int  pidfd_signal_(int pidfd, int sig);

/* procfs.c */
extern const char *proc_root;                         /* "/proc"; unit tests point it at a fixture tree */
extern bool proc_children_scan_only;                  /* tests: skip the per-thread children files, always scan */
struct procent { pid_t pid, ppid; };
struct fdent { int fd; char kind; unsigned long ino; int accmode; }; /* kind: 'p' pipe, 's' socket, 'o' other */
pid_t    proc_ppid(pid_t pid);                        /* -1 if gone */
char     proc_state(pid_t pid);                       /* 'R', 'S', 'T', 'Z', ... or 0 */
int      proc_cmdline(pid_t pid, char *buf, size_t len);
int      proc_children(pid_t parent, pid_t *out, int max);
int      proc_list(struct procent **out);             /* malloc'd, sorted by pid */
bool     proc_is_descendant(pid_t pid, pid_t ancestor);
int      proc_threads(pid_t pid, pid_t *out, int max);
int      proc_fds(pid_t pid, struct fdent *out, int max);
uint64_t proc_sigpending(pid_t pid);                  /* shared + every thread's private set */

/* unixdiag.c */
int           unixdiag_load(void);
unsigned long unixdiag_peer(unsigned long ino);       /* 0 if unknown */

/* inetdiag.c: loopback TCP pairs */
int           inetdiag_load(void);
unsigned long inetdiag_peer(unsigned long ino);       /* 0 unless a loopback peer */
int           inetdiag_endpoints(unsigned long ino, char *buf, size_t len); /* "L <-> R" */
bool          inet_is_loopback(int family, const void *addr);

/* family state, owned by supervisor.c */
struct member {
    pid_t pid, old_ppid;
    char  origin;       /* 'r' started by run, 'a' adopted by cutover, 'o' orphan the kernel handed us */
    bool  alive;
    int   status;
    char  cmd[160];
};

struct stolen {
    int   fd, logfd;    /* fd focker holds; log it drains into (-1 if none) */
    pid_t child, old_parent;
    int   parent_fd, child_fd;
    bool  drain;
    char  kind[8];      /* pipe-r, pipe-w, unix */
    char  log[PATH_MAX + 64];
};

struct family {
    char  name[NAME_MAX_LEN + 1];
    char  dir[PATH_MAX];
    struct member members[MAX_MEMBERS];
    int   nmembers;
    struct stolen stolen[MAX_STOLEN];
    int   nstolen;
};

struct member *family_member(struct family *f, pid_t pid, bool create);

/* cutover.c */
enum freeze_method { FREEZE_PTRACE, FREEZE_SIGNAL, FREEZE_NONE };
struct cutover_opts {
    enum freeze_method freeze;
    int   sig;          /* sent to the old parent; 0 = wait for it to exit */
    int   timeout_ms;
    pid_t from;         /* also cut over every child of this pid */
    bool  steal, drain, suppress, dry_run;
};
int         parse_freeze(const char *s);
const char *freeze_name(enum freeze_method m);
int cutover(struct family *f, const struct cutover_opts *o, const pid_t *pids, int npids, struct sbuf *out);

/* supervisor.c */
int supervisor_run(const char *name, const char *dir, char **argv, bool detached, bool linger, int ready_fd);

/* client.c */
int client_request(const char *name, char **args, int nargs, struct sbuf *reply); /* 0 ok, 1 err, -1 unreachable */
int client_info(const char *name, pid_t *pid, int *nchildren, char *cmd, size_t cmdlen);
int cmd_ps(bool all, bool json);
int cmd_tree(const char *name);
int cmd_logs(const char *name, pid_t adopted, bool follow);
int cmd_rm(const char *name, bool quiet);
int cmd_signal_pids(const char *name, const pid_t *pids, int n, bool stop);

#endif
