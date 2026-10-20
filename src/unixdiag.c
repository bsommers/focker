/* Map each AF_UNIX socket inode to its peer's inode via NETLINK_SOCK_DIAG.
 * The two ends of a socketpair() have different inodes, so this is the only
 * way to tell that the old parent's socket is wired to the child's. */
#include "focker.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/unix_diag.h>

struct pair { unsigned long ino, peer; };
static struct pair *pairs;
static size_t npairs, cap;

static void add_pair(unsigned long ino, unsigned long peer)
{
    if (npairs == cap) {
        size_t ncap = cap ? cap * 2 : 256;
        struct pair *np = realloc(pairs, ncap * sizeof *np);
        if (!np)
            return;
        pairs = np;
        cap = ncap;
    }
    pairs[npairs].ino = ino;
    pairs[npairs].peer = peer;
    npairs++;
}

int unixdiag_load(void)
{
    npairs = 0;
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return -1;

    struct {
        struct nlmsghdr nlh;
        struct unix_diag_req req;
    } msg;
    memset(&msg, 0, sizeof msg);
    msg.nlh.nlmsg_len = sizeof msg;
    msg.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    msg.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    msg.req.sdiag_family = AF_UNIX;
    msg.req.udiag_states = ~0u;
    msg.req.udiag_show = UDIAG_SHOW_PEER;

    struct sockaddr_nl nl = { .nl_family = AF_NETLINK };
    if (sendto(fd, &msg, sizeof msg, 0, (struct sockaddr *)&nl, sizeof nl) < 0)
        goto fail;

    static char buf[32768] __attribute__((aligned(NLMSG_ALIGNTO)));
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            goto fail;
        }
        if (n == 0)
            break;
        int rem = (int)n;
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, rem); h = NLMSG_NEXT(h, rem)) {
            if (h->nlmsg_type == NLMSG_DONE)
                goto done;
            if (h->nlmsg_type == NLMSG_ERROR) {
                errno = EPROTO;
                goto fail;
            }
            struct unix_diag_msg *m = NLMSG_DATA(h);
            int alen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof *m));
            for (struct rtattr *a = (struct rtattr *)(m + 1); RTA_OK(a, alen); a = RTA_NEXT(a, alen))
                if (a->rta_type == UNIX_DIAG_PEER)
                    add_pair(m->udiag_ino, *(uint32_t *)RTA_DATA(a));
        }
    }
done:
    close(fd);
    return 0;
fail: {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
}
}

unsigned long unixdiag_peer(unsigned long ino)
{
    for (size_t i = 0; i < npairs; i++)
        if (pairs[i].ino == ino)
            return pairs[i].peer;
    return 0;
}
