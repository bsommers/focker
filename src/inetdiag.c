/* Pair loopback TCP sockets via NETLINK_SOCK_DIAG (inet_diag). The two ends
 * of a loopback connection are different sockets with different inodes; they
 * are matched by their 4-tuples mirroring each other. Only established
 * sockets whose ends are both loopback addresses are kept, so a dump on a
 * busy host stays small. */
#include "focker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>

#define TCP_ESTABLISHED_STATE 1 /* linux/tcp_states.h */

/* Addresses are kept in IPv6 form (IPv4 as ::ffff:a.b.c.d) so a dual-stack
 * socket talking to 127.0.0.1 still pairs with a plain IPv4 one. */
struct tcpent {
    unsigned long ino;
    int family; /* as reported, for formatting */
    unsigned char laddr[16], raddr[16];
    uint16_t lport, rport; /* host order */
};
static struct tcpent *ents;
static size_t nents, cap;

bool inet_is_loopback(int family, const void *addr)
{
    const unsigned char *a = addr;
    if (family == AF_INET)
        return a[0] == 127;
    if (family != AF_INET6)
        return false;
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (memcmp(a, mapped, 12) == 0)
        return a[12] == 127;
    return memcmp(a, &in6addr_loopback, 16) == 0;
}

static void to_v6(int family, const void *src, unsigned char dst[16])
{
    if (family == AF_INET6) {
        memcpy(dst, src, 16);
        return;
    }
    memset(dst, 0, 10);
    dst[10] = dst[11] = 0xff;
    memcpy(dst + 12, src, 4);
}

static void add_ent(const struct inet_diag_msg *m)
{
    int fam = m->idiag_family;
    if (m->idiag_inode == 0 || !inet_is_loopback(fam, m->id.idiag_src) || !inet_is_loopback(fam, m->id.idiag_dst))
        return;
    if (nents == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        struct tcpent *ne = realloc(ents, ncap * sizeof *ne);
        if (!ne)
            return;
        ents = ne;
        cap = ncap;
    }
    struct tcpent *e = &ents[nents++];
    e->ino = m->idiag_inode;
    e->family = fam;
    to_v6(fam, m->id.idiag_src, e->laddr);
    to_v6(fam, m->id.idiag_dst, e->raddr);
    e->lport = ntohs(m->id.idiag_sport);
    e->rport = ntohs(m->id.idiag_dport);
}

static int dump(int family)
{
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return -1;

    struct {
        struct nlmsghdr nlh;
        struct inet_diag_req_v2 req;
    } msg;
    memset(&msg, 0, sizeof msg);
    msg.nlh.nlmsg_len = sizeof msg;
    msg.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    msg.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    msg.req.sdiag_family = (uint8_t)family;
    msg.req.sdiag_protocol = IPPROTO_TCP;
    msg.req.idiag_states = 1u << TCP_ESTABLISHED_STATE;

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
                const struct nlmsgerr *err = NLMSG_DATA(h);
                errno = err->error < 0 ? -err->error : EPROTO;
                goto fail;
            }
            if (h->nlmsg_len >= NLMSG_LENGTH(sizeof(struct inet_diag_msg)))
                add_ent(NLMSG_DATA(h));
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

int inetdiag_load(void)
{
    nents = 0;
    if (dump(AF_INET) < 0)
        return -1;
    /* A kernel without IPv6 (or without inet6 diag) has no v6 sockets to pair. */
    if (dump(AF_INET6) < 0 && errno != ENOENT && errno != EAFNOSUPPORT && errno != EINVAL)
        return -1;
    return 0;
}

static const struct tcpent *find(unsigned long ino)
{
    for (size_t i = 0; i < nents; i++)
        if (ents[i].ino == ino)
            return &ents[i];
    return NULL;
}

unsigned long inetdiag_peer(unsigned long ino)
{
    const struct tcpent *e = find(ino);
    if (!e)
        return 0;
    for (size_t i = 0; i < nents; i++) {
        const struct tcpent *o = &ents[i];
        if (o != e && o->lport == e->rport && o->rport == e->lport && memcmp(o->laddr, e->raddr, 16) == 0 &&
            memcmp(o->raddr, e->laddr, 16) == 0)
            return o->ino;
    }
    return 0;
}

static void fmt_end(int family, const unsigned char a[16], uint16_t port, char *out, size_t len)
{
    char host[INET6_ADDRSTRLEN];
    if (family == AF_INET) {
        inet_ntop(AF_INET, a + 12, host, sizeof host);
        snprintf(out, len, "%s:%u", host, port);
    } else {
        inet_ntop(AF_INET6, a, host, sizeof host);
        snprintf(out, len, "[%s]:%u", host, port);
    }
}

int inetdiag_endpoints(unsigned long ino, char *buf, size_t len)
{
    const struct tcpent *e = find(ino);
    if (!e || len == 0)
        return -1;
    char l[INET6_ADDRSTRLEN + 8], r[INET6_ADDRSTRLEN + 8];
    fmt_end(e->family, e->laddr, e->lport, l, sizeof l);
    fmt_end(e->family, e->raddr, e->rport, r, sizeof r);
    snprintf(buf, len, "%s <-> %s", l, r);
    return 0;
}
