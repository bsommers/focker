/* ticker: the child. Prints a numbered line every interval, along with its
 * current parent, so a reparent shows up in its own output.
 *
 *   ticker [-n COUNT] [-i MS] [-d SIG] [-t THREADS]
 *     -n COUNT    stop after COUNT ticks (default: forever)
 *     -i MS       interval in milliseconds (default 500)
 *     -d SIG      set PR_SET_PDEATHSIG to signal number SIG (to show the gotcha)
 *     -t THREADS  start THREADS idle extra threads (to exercise freezing them all)
 *
 * SIGPIPE is left at its default, so if the reader of stdout goes away
 * (the old parent dies and nobody took over the pipe), ticker dies. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

static void *idle(void *arg)
{
    (void)arg;
    for (;;)
        pause();
    return NULL;
}

int main(int argc, char **argv)
{
    long count = -1, interval = 500;
    int c, deathsig = 0, threads = 0;
    while ((c = getopt(argc, argv, "n:i:d:t:")) != -1) {
        switch (c) {
        case 'n': count = atol(optarg); break;
        case 'i': interval = atol(optarg); break;
        case 'd': deathsig = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: ticker [-n COUNT] [-i MS] [-d SIG] [-t THREADS]\n");
            return 2;
        }
    }
    if (deathsig && prctl(PR_SET_PDEATHSIG, deathsig) < 0) {
        perror("ticker: prctl");
        return 1;
    }
    for (int i = 0; i < threads; i++) {
        pthread_t t;
        pthread_create(&t, NULL, idle, NULL);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (long i = 1; count < 0 || i <= count; i++) {
        printf("tick %ld pid=%d ppid=%d\n", i, getpid(), getppid());
        struct timespec ts = { interval / 1000, (interval % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}
