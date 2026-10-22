/* Pseudo-terminals a family owns: the supervisor holds the master and drains
 * it to the family log; the job gets the slave as its controlling terminal. */
#include "focker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

int pty_open(int *master, int *slave, const struct termios *tio, const struct winsize *ws)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (m < 0)
        return -1;
    const char *path;
    if (grantpt(m) < 0 || unlockpt(m) < 0 || !(path = ptsname(m)))
        goto fail;
    int s = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (s < 0)
        goto fail;
    if ((tio && tcsetattr(s, TCSANOW, tio) < 0) || (ws && ioctl(s, TIOCSWINSZ, ws) < 0)) {
        int e = errno;
        close(s);
        errno = e;
        goto fail;
    }
    *master = m;
    *slave = s;
    return 0;
fail:;
    int e = errno;
    close(m);
    errno = e;
    return -1;
}

int pty_take_ctty(int slave)
{
    return ioctl(slave, TIOCSCTTY, 0) < 0 ? -1 : 0;
}
