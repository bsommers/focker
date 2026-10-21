/* oldparent: the parent being replaced. Starts N tickers with their stdout
 * piped back to it, and relays each line with its own tag. When it dies, the
 * read ends of those pipes die with it unless someone takes them over.
 *
 *   oldparent [-s] [N] [-- TICKER-ARGS...]
 *     -s  connect each ticker's stdout with socketpair() instead of pipe()
 *
 * TICKER is found next to this binary. */
#include <errno.h>
#include <stdbool.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAXKIDS 128

int main(int argc, char **argv)
{
    int n = 1, first_extra = argc, a = 1;
    bool sock = false, stdin_pipe = false;
    for (; a < argc && argv[a][0] == '-' && strcmp(argv[a], "--") != 0; a++) {
        if (strcmp(argv[a], "-s") == 0)
            sock = true;
        else if (strcmp(argv[a], "-I") == 0)
            stdin_pipe = true; /* also hold the write end of each child's stdin */
        else
            break;
    }
    if (a < argc && strcmp(argv[a], "--") != 0)
        n = atoi(argv[a]);
    for (int i = a; i < argc; i++)
        if (strcmp(argv[i], "--") == 0) {
            first_extra = i + 1;
            break;
        }
    if (n < 1 || n > MAXKIDS) {
        fprintf(stderr, "usage: oldparent [-s] [-I] [N (1-%d)] [-- TICKER-ARGS...]\n", MAXKIDS);
        return 2;
    }

    char self[PATH_MAX], ticker[PATH_MAX + 8];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l < 0) {
        perror("oldparent: readlink");
        return 1;
    }
    self[l] = '\0';
    *strrchr(self, '/') = '\0';
    snprintf(ticker, sizeof ticker, "%s/ticker", self);

    setvbuf(stdout, NULL, _IOLBF, 0);
    struct pollfd fds[MAXKIDS];
    FILE *in[MAXKIDS];
    for (int i = 0; i < n; i++) {
        int p[2];
        int r = sock ? socketpair(AF_UNIX, SOCK_STREAM, 0, p) : pipe(p);
        int q[2] = { -1, -1 };
        if (r < 0 || (stdin_pipe && pipe(q) < 0)) {
            perror("oldparent: pipe");
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("oldparent: fork");
            return 1;
        }
        if (pid == 0) {
            if (stdin_pipe) {
                dup2(q[0], 0);
                close(q[0]);
                close(q[1]);
            }
            dup2(p[1], 1);
            close(p[0]);
            close(p[1]);
            char *args[64] = { ticker };
            int k = 1;
            for (int j = first_extra; j < argc && k < 63; j++)
                args[k++] = argv[j];
            args[k] = NULL;
            execv(ticker, args);
            perror("oldparent: exec ticker");
            _exit(127);
        }
        close(p[1]);
        if (stdin_pipe)
            close(q[0]); /* keep q[1] and never write: the child's stdin stays open */
        fds[i] = (struct pollfd){ .fd = p[0], .events = POLLIN };
        in[i] = fdopen(p[0], "r");
        printf("[oldparent %d] started ticker %d\n", getpid(), pid);
    }

    int open_pipes = n;
    char line[256];
    while (open_pipes > 0) {
        if (poll(fds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            perror("oldparent: poll");
            return 1;
        }
        for (int i = 0; i < n; i++) {
            if (!fds[i].revents)
                continue;
            if (fgets(line, sizeof line, in[i])) {
                printf("[oldparent %d] %s", getpid(), line);
            } else {
                fclose(in[i]);
                fds[i].fd = -1;
                open_pipes--;
            }
        }
    }
    return 0;
}
