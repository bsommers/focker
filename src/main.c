/* focker: command-line front end. Parses arguments and calls into client.c
 * (most commands) or supervisor.c (run). */
#include "focker.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *USAGE =
    "usage: focker <command> [options] [args]\n"
    "\n"
    "Move running processes onto a new parent, cleanly. Think docker, but it's meet the Fockers.\n"
    "\n"
    "commands:\n"
    "  run       start a family: focker becomes a subreaper and runs CMD as its first parent\n"
    "  cutover   adopt processes away from their parent: freeze, take over fds, cut ties, thaw\n"
    "  handover  replace focker with a new parent program that inherits every adopted child\n"
    "  ps        list families\n"
    "  tree      show a family's process tree\n"
    "  inspect   show a family's members and the fds it holds\n"
    "  logs      print a family's log, or an adopted child's captured output\n"
    "  fg        attach this terminal to a family's terminal (Ctrl-P Ctrl-Q detaches)\n"
    "  adopt-external  move a job from another terminal into a family (needs criu)\n"
    "  freeze    stop processes in a family with SIGSTOP\n"
    "  thaw      resume processes in a family with SIGCONT\n"
    "  stop      signal a family's direct children (default SIGTERM)\n"
    "  release   close fds focker holds for adopted children (e.g. to send EOF)\n"
    "  rm        remove an exited family's state directory\n"
    "  version   print the version\n"
    "\n"
    "Run 'focker <command> --help' for a command's options.\n"
    "Exit codes: 0 success, 1 failure, 2 usage error.\n";

static const char *RUN_USAGE =
    "usage: focker run [-n NAME] [-d [-t]] [-l] [--] CMD [ARG...]\n"
    "\n"
    "Start a family. focker marks itself PR_SET_CHILD_SUBREAPER and runs CMD as its\n"
    "child (the first, \"old\" parent). Anything orphaned below it lands on focker.\n"
    "\n"
    "  -n, --name NAME   family name (default: random, like suspicious_jack)\n"
    "  -d, --detach      run in the background; output goes to 'focker logs NAME'\n"
    "  -t, --tty         with -d: run CMD on a terminal that focker holds\n"
    "  -l, --linger      keep running when the family has no processes left\n"
    "  -h, --help        show this help\n"
    "\n"
    "Without -d, focker exits with CMD's exit status once the whole family is gone.\n"
    "\n"
    "example:\n"
    "  focker run -d -n demo -- ./build/oldparent 2\n";

static const char *CUTOVER_USAGE =
    "usage: focker cutover [options] NAME PID...\n"
    "       focker cutover [options] --from PPID NAME\n"
    "\n"
    "Adopt PIDs away from their current parent. The steps are: freeze each PID,\n"
    "take over the parent's ends of its pipes, unix sockets and loopback TCP\n"
    "connections, make the old parent exit, check the PIDs were adopted by focker,\n"
    "then thaw them and swallow any signal sent to them in the meantime.\n"
    "\n"
    "  -f, --freeze METHOD  ptrace (default; invisible to the child, can swallow signals),\n"
    "                       signal (SIGSTOP/SIGCONT) or none  [env FOCKER_FREEZE]\n"
    "  -s, --signal SIG     signal for the old parent (default TERM)\n"
    "  -w, --wait           don't signal the old parent; wait for it to exit on its own\n"
    "  -t, --timeout SEC    give up and roll back after SEC seconds (default 10)\n"
    "      --from PPID      also adopt every child of PPID\n"
    "      --no-steal       leave the old parent's pipes and sockets alone\n"
    "      --no-drain       hold taken-over read ends open without reading them\n"
    "      --no-suppress    deliver signals that arrived during the cutover\n"
    "  -n, --dry-run        print the plan and change nothing\n"
    "  -h, --help           show this help\n"
    "\n"
    "Killing the old parent orphans ALL of its children. Children you didn't name are\n"
    "adopted as well, but they are not frozen first.\n"
    "\n"
    "examples:\n"
    "  focker cutover --dry-run demo 4242\n"
    "  focker cutover demo 4242\n"
    "  focker cutover --from 4241 --signal KILL demo\n";

static const char *HANDOVER_USAGE =
    "usage: focker handover NAME [--] CMD [ARG...]\n"
    "\n"
    "Replace the family's supervisor with CMD by calling execve(). The PID does not\n"
    "change, so every child focker holds becomes a real child of CMD, and CMD can\n"
    "waitpid() for it. Held fds survive the exec. CMD is told about them through:\n"
    "  FOCKER_CHILDREN   comma-separated child pids\n"
    "  FOCKER_FDS        comma-separated FD:CHILD:KIND:CHILDFD (KIND is pipe-r, pipe-w, unix or tcp)\n"
    "  FOCKER_NAME       the family name\n"
    "  FOCKER_STATE_DIR  the family's state directory\n"
    "After the handover the family no longer answers focker commands.\n"
    "\n"
    "example:\n"
    "  focker handover demo -- ./build/newparent\n";

static const char *PS_USAGE =
    "usage: focker ps [-a] [--json]\n"
    "\n"
    "  -a, --all   include exited families\n"
    "  --json      print one JSON document instead of the table (keys in docs/USAGE.md)\n";

static const char *RELEASE_USAGE =
    "usage: focker release NAME FD...\n"
    "\n"
    "Closes fds focker holds on behalf of adopted children. FD is focker's own fd\n"
    "number: the FD column of 'focker inspect', or the first field of FOCKER_FDS.\n"
    "Closing a held pipe-w sends the child EOF on that fd; closing a drained fd\n"
    "also closes its log. Exits 1 if any FD isn't held (the others are still closed).\n";

static const char *NAME_USAGE_FMT =
    "usage: focker %s NAME\n";

static const char *INSPECT_USAGE =
    "usage: focker inspect [--json] NAME\n"
    "\n"
    "  --json   print one JSON document instead of the report (keys in docs/USAGE.md)\n";

static const char *LOGS_USAGE =
    "usage: focker logs [-f] [-p PID] NAME\n"
    "\n"
    "Print the family's log. In detached mode the log holds the first command's output\n"
    "and focker's own messages.\n"
    "\n"
    "  -f, --follow    keep printing as the log grows, until the family exits\n"
    "  -p, --pid PID   print output drained from PID's taken-over pipes instead\n";

static const char *SIGNAL_USAGE_FMT =
    "usage: focker %s NAME PID...\n"
    "\n"
    "%s each PID, which must belong to family NAME.\n";

static const char *STOP_USAGE =
    "usage: focker stop [-s SIG] NAME\n"
    "\n"
    "Send SIG (default TERM) to the family's direct children. The family exits\n"
    "once it has no processes left.\n";

static int usage_error(const char *usage, const char *fmt, const char *arg)
{
    if (fmt) {
        fprintf(stderr, "focker: ");
        fprintf(stderr, fmt, arg);
        fprintf(stderr, "\n");
    }
    fputs(usage, stderr);
    return FK_USAGE;
}

static int parse_pid(const char *s, pid_t *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v <= 0 || v > 4194304)
        return -1;
    *out = (pid_t)v;
    return 0;
}

static int need_name(const char *name, const char *usage)
{
    if (!valid_name(name))
        return usage_error(usage, "invalid family name '%s' (letters, digits, '_', '-', '.'; at most 48)", name);
    return FK_OK;
}

/* ---- run ---------------------------------------------------------------- */

static void random_name(char *buf, size_t len)
{
    static const char *adj[] = { "suspicious", "nervous", "trusting", "proud", "cranky", "loyal",
                                 "flushed", "gentle", "wary", "smug", "sunny", "polygraphed" };
    static const char *who[] = { "greg", "pam", "jack", "dina", "bernie", "roz",
                                 "kevin", "jinx", "moses", "debbie", "larry", "denny" };
    srand((unsigned)(time(NULL) ^ getpid()));
    for (int tries = 0; tries < 100; tries++) {
        snprintf(buf, len, "%s_%s", adj[rand() % 12], who[rand() % 12]);
        char dir[PATH_MAX];
        if (family_dir(buf, dir, sizeof dir) == 0 && access(dir, F_OK) < 0)
            return;
    }
    snprintf(buf, len, "family_%d", (int)getpid());
}

static int cmd_run(int argc, char **argv)
{
    static const struct option opts[] = { { "name", required_argument, 0, 'n' },
                                           { "detach", no_argument, 0, 'd' },
                                           { "linger", no_argument, 0, 'l' },
                                           { "tty", no_argument, 0, 't' },
                                           { "help", no_argument, 0, 'h' },
                                           { 0, 0, 0, 0 } };
    const char *name = NULL;
    bool detach = false, linger = false, tty = false;
    int c;
    while ((c = getopt_long(argc, argv, "+n:dlth", opts, NULL)) != -1) {
        switch (c) {
        case 'n': name = optarg; break;
        case 'd': detach = true; break;
        case 'l': linger = true; break;
        case 't': tty = true; break;
        case 'h': fputs(RUN_USAGE, stdout); return FK_OK;
        default: return usage_error(RUN_USAGE, NULL, NULL);
        }
    }
    if (optind >= argc)
        return usage_error(RUN_USAGE, "missing command%s", "");
    if (tty && !detach)
        return usage_error(RUN_USAGE, "-t needs -d (a foreground family already has your terminal)%s", "");
    char gen[NAME_MAX_LEN + 1];
    if (!name) {
        random_name(gen, sizeof gen);
        name = gen;
    } else if (need_name(name, RUN_USAGE) != FK_OK) {
        return FK_USAGE;
    }

    char dir[PATH_MAX];
    if (family_dir(name, dir, sizeof dir) < 0) {
        fprintf(stderr, "focker: cannot use runtime dir: %s\n", strerror(errno));
        return FK_FAIL;
    }
    if (mkdir(dir, 0700) < 0) {
        if (errno != EEXIST) {
            fprintf(stderr, "focker: %s: %s\n", dir, strerror(errno));
            return FK_FAIL;
        }
        if (client_info(name, NULL, NULL, NULL, 0) == 0)
            fprintf(stderr, "focker: family '%s' is already running\n", name);
        else
            fprintf(stderr, "focker: name '%s' belongs to an exited family; 'focker rm %s' first\n", name, name);
        return FK_FAIL;
    }
    char **cmd = argv + optind;

    if (!detach) {
        int rc = supervisor_run(name, dir, cmd, false, linger, false, -1);
        if (rc == 127) /* never started: free the name */
            cmd_rm(name, true);
        return rc;
    }

    int ready[2];
    if (pipe2(ready, O_CLOEXEC) < 0) {
        fprintf(stderr, "focker: pipe: %s\n", strerror(errno));
        return FK_FAIL;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "focker: fork: %s\n", strerror(errno));
        close(ready[0]);
        close(ready[1]);
        return FK_FAIL;
    }
    if (pid == 0) {
        close(ready[0]);
        setsid();
        char logpath[PATH_MAX + 16];
        snprintf(logpath, sizeof logpath, "%s/focker.log", dir);
        int log = open(logpath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (log < 0 || null < 0)
            _exit(FK_FAIL);
        dup2(null, 0);
        dup2(log, 1);
        dup2(log, 2);
        setvbuf(stdout, NULL, _IOLBF, 0);
        _exit(supervisor_run(name, dir, cmd, true, linger, tty, ready[1]));
    }
    close(ready[1]);
    char b;
    ssize_t n;
    while ((n = read(ready[0], &b, 1)) < 0 && errno == EINTR)
        ;
    close(ready[0]);
    if (n != 1) {
        char logpath[PATH_MAX + 16], line[512] = "";
        snprintf(logpath, sizeof logpath, "%s/focker.log", dir);
        FILE *f = fopen(logpath, "re");
        while (f && fgets(line, sizeof line, f))
            ;
        if (f)
            fclose(f);
        fprintf(stderr, "focker: family '%s' failed to start%s%s", name, line[0] ? ": " : "\n", line);
        cmd_rm(name, true);
        return FK_FAIL;
    }
    printf("%s\n", name);
    return FK_OK;
}

/* ---- cutover ------------------------------------------------------------ */

static int cmd_cutover(int argc, char **argv)
{
    enum { O_FROM = 256, O_NOSTEAL, O_NODRAIN, O_NOSUPPRESS };
    static const struct option opts[] = { { "freeze", required_argument, 0, 'f' },
                                           { "signal", required_argument, 0, 's' },
                                           { "wait", no_argument, 0, 'w' },
                                           { "timeout", required_argument, 0, 't' },
                                           { "from", required_argument, 0, O_FROM },
                                           { "no-steal", no_argument, 0, O_NOSTEAL },
                                           { "no-drain", no_argument, 0, O_NODRAIN },
                                           { "no-suppress", no_argument, 0, O_NOSUPPRESS },
                                           { "dry-run", no_argument, 0, 'n' },
                                           { "help", no_argument, 0, 'h' },
                                           { 0, 0, 0, 0 } };
    const char *env = getenv("FOCKER_FREEZE");
    int freeze = FREEZE_PTRACE, sig = SIGTERM;
    if (env && *env && (freeze = parse_freeze(env)) < 0)
        return usage_error(CUTOVER_USAGE, "FOCKER_FREEZE='%s' is not ptrace, signal or none", env);
    double timeout = 10;
    pid_t from = 0;
    bool steal = true, drain = true, suppress = true, dry = false;
    int c;
    while ((c = getopt_long(argc, argv, "+f:s:wt:nh", opts, NULL)) != -1) {
        switch (c) {
        case 'f':
            if ((freeze = parse_freeze(optarg)) < 0)
                return usage_error(CUTOVER_USAGE, "--freeze must be ptrace, signal or none, not '%s'", optarg);
            break;
        case 's':
            if ((sig = parse_signal(optarg)) < 0)
                return usage_error(CUTOVER_USAGE, "unknown signal '%s'", optarg);
            break;
        case 'w': sig = 0; break;
        case 't': {
            char *end;
            timeout = strtod(optarg, &end);
            if (*end || timeout < 0.1 || timeout > 3600)
                return usage_error(CUTOVER_USAGE, "--timeout must be between 0.1 and 3600 seconds, not '%s'",
                                   optarg);
            break;
        }
        case O_FROM:
            if (parse_pid(optarg, &from) < 0)
                return usage_error(CUTOVER_USAGE, "invalid pid '%s'", optarg);
            break;
        case O_NOSTEAL: steal = false; break;
        case O_NODRAIN: drain = false; break;
        case O_NOSUPPRESS: suppress = false; break;
        case 'n': dry = true; break;
        case 'h': fputs(CUTOVER_USAGE, stdout); return FK_OK;
        default: return usage_error(CUTOVER_USAGE, NULL, NULL);
        }
    }
    if (optind >= argc)
        return usage_error(CUTOVER_USAGE, "missing family name%s", "");
    const char *name = argv[optind++];
    if (need_name(name, CUTOVER_USAGE) != FK_OK)
        return FK_USAGE;
    if (optind >= argc && !from)
        return usage_error(CUTOVER_USAGE, "name at least one PID, or use --from PPID%s", "");
    if (argc - optind > MAX_PIDS)
        return usage_error(CUTOVER_USAGE, "at most 64 PIDs per cutover%s", "");

    static char tok[16 + MAX_PIDS][32];
    char *args[16 + MAX_PIDS];
    int n = 0;
    snprintf(tok[n], 32, "cutover");
    snprintf(tok[++n], 32, "freeze=%s", freeze_name((enum freeze_method)freeze));
    snprintf(tok[++n], 32, "signal=%d", sig);
    snprintf(tok[++n], 32, "timeout=%d", (int)(timeout * 1000));
    snprintf(tok[++n], 32, "from=%d", from);
    snprintf(tok[++n], 32, "steal=%d", steal);
    snprintf(tok[++n], 32, "drain=%d", drain);
    snprintf(tok[++n], 32, "suppress=%d", suppress);
    snprintf(tok[++n], 32, "dry_run=%d", dry);
    n++;
    for (; optind < argc; optind++) {
        pid_t pid;
        if (parse_pid(argv[optind], &pid) < 0)
            return usage_error(CUTOVER_USAGE, "invalid pid '%s'", argv[optind]);
        snprintf(tok[n++], 32, "pid=%d", pid);
    }
    for (int i = 0; i < n; i++)
        args[i] = tok[i];

    struct sbuf r = { 0 };
    int rc = client_request(name, args, n, &r);
    if (rc < 0) {
        fprintf(stderr, "focker: no running family '%s' (%s)\n", name, strerror(errno));
        return FK_FAIL;
    }
    fputs(r.p ? r.p : "", rc == 0 ? stdout : stderr);
    sb_free(&r);
    return rc == 0 ? FK_OK : FK_FAIL;
}

/* ---- small commands -------------------------------------------------------- */

static int simple_request(const char *name, char **args, int nargs)
{
    struct sbuf r = { 0 };
    int rc = client_request(name, args, nargs, &r);
    if (rc < 0) {
        fprintf(stderr, "focker: no running family '%s' (%s)\n", name, strerror(errno));
        return FK_FAIL;
    }
    fputs(r.p ? r.p : "", rc == 0 ? stdout : stderr);
    sb_free(&r);
    return rc == 0 ? FK_OK : FK_FAIL;
}

static bool wants_help(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0)
            return false;
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            return true;
    }
    return false;
}

static int cmd_handover(int argc, char **argv)
{
    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fputs(HANDOVER_USAGE, stdout);
        return FK_OK;
    }
    if (argc < 3)
        return usage_error(HANDOVER_USAGE, "need a family name and a command%s", "");
    const char *name = argv[1];
    if (need_name(name, HANDOVER_USAGE) != FK_OK)
        return FK_USAGE;
    int first = 2;
    if (strcmp(argv[first], "--") == 0)
        first++;
    if (first >= argc)
        return usage_error(HANDOVER_USAGE, "missing command%s", "");
    char *args[MAX_ARGS];
    int n = 0;
    args[n++] = "handover";
    for (int i = first; i < argc && n < MAX_ARGS; i++)
        args[n++] = argv[i];
    return simple_request(name, args, n);
}

static int cmd_ps_main(int argc, char **argv)
{
    bool all = false, json = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0)
            all = true;
        else if (strcmp(argv[i], "--json") == 0)
            json = true;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fputs(PS_USAGE, stdout);
            return FK_OK;
        } else
            return usage_error(PS_USAGE, "unexpected argument '%s'", argv[i]);
    }
    return cmd_ps(all, json);
}

static int cmd_inspect(int argc, char **argv)
{
    bool json = false;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)
            json = true;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fputs(INSPECT_USAGE, stdout);
            return FK_OK;
        } else if (!name)
            name = argv[i];
        else
            return usage_error(INSPECT_USAGE, "unexpected argument '%s'", argv[i]);
    }
    if (!name)
        return usage_error(INSPECT_USAGE, "missing family name%s", "");
    if (need_name(name, INSPECT_USAGE) != FK_OK)
        return FK_USAGE;
    char *args[] = { "inspect", "json=1" };
    return simple_request(name, args, json ? 2 : 1);
}

static const char *FG_USAGE =
    "usage: focker fg NAME\n"
    "\n"
    "Attach this terminal to the terminal of family NAME (one started with 'run -d -t'):\n"
    "you see the job's output, and your keys, Ctrl-C and window size reach it.\n"
    "One terminal at a time: a second 'fg' takes the job over and the first exits.\n"
    "\n"
    "  Ctrl-P Ctrl-Q     detach; the job keeps running and its output goes to 'focker logs NAME'\n"
    "  -h, --help        show this help\n"
    "\n"
    "When the job ends, fg exits with its exit status (128+N if killed by signal N).\n";

static int cmd_fg_main(int argc, char **argv)
{
    if (wants_help(argc, argv)) {
        fputs(FG_USAGE, stdout);
        return FK_OK;
    }
    if (argc != 2)
        return usage_error(FG_USAGE, "expected exactly one family name%s", "");
    if (need_name(argv[1], FG_USAGE) != FK_OK)
        return FK_USAGE;
    return cmd_fg(argv[1]);
}

static const char *ADOPT_USAGE =
    "usage: focker adopt-external [-y] [--dry-run] NAME PID\n"
    "\n"
    "Move the job containing PID (its whole process group, so a pipeline moves together),\n"
    "started in another terminal, into family NAME, using criu to checkpoint it and\n"
    "restore it as focker's child. NAME is joined if running, else started (detached,\n"
    "lingering). A job on a terminal gets a new one focker holds: its output goes to\n"
    "'focker logs NAME' until 'focker fg NAME'.\n"
    "\n"
    "Everything that can be checked is checked first, with a trial dump while the job\n"
    "keeps running; then you are asked before the original is stopped and killed.\n"
    "\n"
    "  -y, --yes         don't ask\n"
    "      --dry-run     check and print the plan; change nothing\n"
    "  -h, --help        show this help\n"
    "\n"
    "Needs criu with CAP_CHECKPOINT_RESTORE (sudo setcap cap_checkpoint_restore+eip $(command -v criu)),\n"
    "and, under Yama ptrace_scope 1, kernel.yama.ptrace_scope=0. $FOCKER_CRIU picks the criu binary.\n";

static int cmd_adopt_external_main(int argc, char **argv)
{
    static const struct option opts[] = { { "yes", no_argument, 0, 'y' },
                                           { "dry-run", no_argument, 0, 'D' },
                                           { "help", no_argument, 0, 'h' },
                                           { 0, 0, 0, 0 } };
    bool yes = false, dry_run = false;
    int c;
    while ((c = getopt_long(argc, argv, "yh", opts, NULL)) != -1) {
        switch (c) {
        case 'y': yes = true; break;
        case 'D': dry_run = true; break;
        case 'h': fputs(ADOPT_USAGE, stdout); return FK_OK;
        default: return usage_error(ADOPT_USAGE, NULL, NULL);
        }
    }
    if (argc - optind != 2)
        return usage_error(ADOPT_USAGE, "expected a family name and a pid%s", "");
    if (need_name(argv[optind], ADOPT_USAGE) != FK_OK)
        return FK_USAGE;
    pid_t pid;
    if (parse_pid(argv[optind + 1], &pid) < 0)
        return usage_error(ADOPT_USAGE, "bad pid '%s'", argv[optind + 1]);
    return cmd_adopt_external(argv[optind], pid, yes, dry_run);
}

static int cmd_named(int argc, char **argv)
{
    char usage[128];
    snprintf(usage, sizeof usage, NAME_USAGE_FMT, argv[0]);
    if (wants_help(argc, argv)) {
        fputs(usage, stdout);
        return FK_OK;
    }
    if (argc != 2)
        return usage_error(usage, "expected exactly one family name%s", "");
    if (need_name(argv[1], usage) != FK_OK)
        return FK_USAGE;
    if (strcmp(argv[0], "tree") == 0)
        return cmd_tree(argv[1]);
    return cmd_rm(argv[1], false);
}

static int cmd_logs_main(int argc, char **argv)
{
    static const struct option opts[] = { { "follow", no_argument, 0, 'f' },
                                           { "pid", required_argument, 0, 'p' },
                                           { "help", no_argument, 0, 'h' },
                                           { 0, 0, 0, 0 } };
    bool follow = false;
    pid_t pid = 0;
    int c;
    while ((c = getopt_long(argc, argv, "fp:h", opts, NULL)) != -1) {
        switch (c) {
        case 'f': follow = true; break;
        case 'p':
            if (parse_pid(optarg, &pid) < 0)
                return usage_error(LOGS_USAGE, "invalid pid '%s'", optarg);
            break;
        case 'h': fputs(LOGS_USAGE, stdout); return FK_OK;
        default: return usage_error(LOGS_USAGE, NULL, NULL);
        }
    }
    if (argc - optind != 1)
        return usage_error(LOGS_USAGE, "expected exactly one family name%s", "");
    if (need_name(argv[optind], LOGS_USAGE) != FK_OK)
        return FK_USAGE;
    return cmd_logs(argv[optind], pid, follow);
}

static int cmd_freeze_thaw(int argc, char **argv)
{
    bool stop = strcmp(argv[0], "freeze") == 0;
    char usage[256];
    snprintf(usage, sizeof usage, SIGNAL_USAGE_FMT, argv[0], stop ? "SIGSTOP" : "SIGCONT");
    if (wants_help(argc, argv)) {
        fputs(usage, stdout);
        return FK_OK;
    }
    if (argc < 3)
        return usage_error(usage, "need a family name and at least one PID%s", "");
    if (need_name(argv[1], usage) != FK_OK)
        return FK_USAGE;
    pid_t pids[MAX_PIDS];
    int n = 0;
    for (int i = 2; i < argc; i++) {
        if (n == MAX_PIDS)
            return usage_error(usage, "at most 64 PIDs%s", "");
        if (parse_pid(argv[i], &pids[n++]) < 0)
            return usage_error(usage, "invalid pid '%s'", argv[i]);
    }
    return cmd_signal_pids(argv[1], pids, n, stop);
}

static int cmd_release(int argc, char **argv)
{
    static const struct option opts[] = { { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 } };
    int c;
    while ((c = getopt_long(argc, argv, "+h", opts, NULL)) != -1) {
        switch (c) {
        case 'h': fputs(RELEASE_USAGE, stdout); return FK_OK;
        default: return usage_error(RELEASE_USAGE, NULL, NULL);
        }
    }
    if (argc - optind < 2)
        return usage_error(RELEASE_USAGE, "expected a family name and at least one FD%s", "");
    if (need_name(argv[optind], RELEASE_USAGE) != FK_OK)
        return FK_USAGE;
    char *args[MAX_ARGS];
    int n = 0;
    args[n++] = "release";
    for (int i = optind + 1; i < argc && n < MAX_ARGS; i++) {
        char *end;
        long fd = strtol(argv[i], &end, 10);
        if (*argv[i] == '\0' || *end || fd < 0 || fd > 65535)
            return usage_error(RELEASE_USAGE, "bad FD '%s'", argv[i]);
        args[n++] = argv[i];
    }
    return simple_request(argv[optind], args, n);
}

static int cmd_stop(int argc, char **argv)
{
    static const struct option opts[] = { { "signal", required_argument, 0, 's' },
                                           { "help", no_argument, 0, 'h' },
                                           { 0, 0, 0, 0 } };
    int sig = SIGTERM, c;
    while ((c = getopt_long(argc, argv, "s:h", opts, NULL)) != -1) {
        switch (c) {
        case 's':
            if ((sig = parse_signal(optarg)) < 0)
                return usage_error(STOP_USAGE, "unknown signal '%s'", optarg);
            break;
        case 'h': fputs(STOP_USAGE, stdout); return FK_OK;
        default: return usage_error(STOP_USAGE, NULL, NULL);
        }
    }
    if (argc - optind != 1)
        return usage_error(STOP_USAGE, "expected exactly one family name%s", "");
    if (need_name(argv[optind], STOP_USAGE) != FK_OK)
        return FK_USAGE;
    char tok[32];
    snprintf(tok, sizeof tok, "signal=%d", sig);
    char *args[] = { "stop", tok };
    return simple_request(argv[optind], args, 2);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage_error(USAGE, NULL, NULL);
    const char *cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        fputs(USAGE, stdout);
        return FK_OK;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("focker %s\n", FOCKER_VERSION);
        return FK_OK;
    }
    argc--;
    argv++;
    if (strcmp(cmd, "run") == 0)
        return cmd_run(argc, argv);
    if (strcmp(cmd, "cutover") == 0 || strcmp(cmd, "adopt") == 0)
        return cmd_cutover(argc, argv);
    if (strcmp(cmd, "handover") == 0 || strcmp(cmd, "meet-the-parents") == 0)
        return cmd_handover(argc, argv);
    if (strcmp(cmd, "ps") == 0)
        return cmd_ps_main(argc, argv);
    if (strcmp(cmd, "inspect") == 0)
        return cmd_inspect(argc, argv);
    if (strcmp(cmd, "tree") == 0 || strcmp(cmd, "rm") == 0)
        return cmd_named(argc, argv);
    if (strcmp(cmd, "logs") == 0)
        return cmd_logs_main(argc, argv);
    if (strcmp(cmd, "fg") == 0)
        return cmd_fg_main(argc, argv);
    if (strcmp(cmd, "adopt-external") == 0)
        return cmd_adopt_external_main(argc, argv);
    if (strcmp(cmd, "freeze") == 0 || strcmp(cmd, "thaw") == 0)
        return cmd_freeze_thaw(argc, argv);
    if (strcmp(cmd, "stop") == 0)
        return cmd_stop(argc, argv);
    if (strcmp(cmd, "release") == 0)
        return cmd_release(argc, argv);
    fprintf(stderr, "focker: unknown command '%s'\n", cmd);
    fputs(USAGE, stderr);
    return FK_USAGE;
}
