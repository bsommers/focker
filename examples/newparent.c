/* newparent: the program 'focker handover' execs into. Reads FOCKER_CHILDREN
 * and FOCKER_FDS, keeps relaying the adopted tickers' output from the
 * inherited pipe read ends, and reaps the children itself, since they are
 * now its real children. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAXFDS 64

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *kids = getenv("FOCKER_CHILDREN");
    const char *fdlist = getenv("FOCKER_FDS");
    printf("[newparent %d] FOCKER_CHILDREN=%s\n", getpid(), kids ? kids : "");
    printf("[newparent %d] FOCKER_FDS=%s\n", getpid(), fdlist ? fdlist : "");

    struct pollfd fds[MAXFDS];
    int n = 0;
    char *copy = strdup(fdlist ? fdlist : "");
    for (char *tok = strtok(copy, ","); tok && n < MAXFDS; tok = strtok(NULL, ",")) {
        int fd, child, childfd;
        char kind[16];
        if (sscanf(tok, "%d:%d:%15[^:]:%d", &fd, &child, kind, &childfd) != 4)
            continue;
        if (strcmp(kind, "pipe-r") == 0 || strcmp(kind, "unix") == 0)
            fds[n++] = (struct pollfd){ .fd = fd, .events = POLLIN };
    }
    free(copy);

    int live = 0;
    for (;;) {
        int st;
        pid_t pid;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
            printf("[newparent %d] reaped %d (%s %d)\n", getpid(), pid,
                   WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st));
        if (pid < 0 && errno == ECHILD) {
            live = 0;
            for (int i = 0; i < n; i++)
                live += fds[i].fd >= 0;
            if (!live)
                break;
        }
        if (poll(fds, (nfds_t)n, 200) <= 0)
            continue;
        for (int i = 0; i < n; i++) {
            if (!fds[i].revents)
                continue;
            char buf[4096];
            ssize_t r = read(fds[i].fd, buf, sizeof buf);
            if (r <= 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
                continue;
            }
            /* re-tag each line */
            for (char *line = buf; line < buf + r;) {
                char *nl = memchr(line, '\n', (size_t)(buf + r - line));
                size_t len = nl ? (size_t)(nl - line + 1) : (size_t)(buf + r - line);
                printf("[newparent %d] %.*s%s", getpid(), (int)len, line, nl ? "" : "\n");
                line += len;
            }
        }
    }
    printf("[newparent %d] all children gone, exiting\n", getpid());
    return 0;
}
