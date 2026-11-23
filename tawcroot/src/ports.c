/* Privileged ports for `--init` sessions. An app can't bind ports below
 * 1024, but services do (systemd-resolved's stub on 127.0.0.53:53).
 * With no network namespace, guest port p < 1024 maps to host port
 * p + TAWCROOT_PORT_SHIFT: binds shift on any address, connects and
 * sends shift for loopback destinations, and names coming back
 * (getsockname, getpeername, accept, recvfrom/recvmsg sources) shift
 * back. Datagram sockets live above the reserved fd floor, where a
 * stacked filter traps their receives; stream receives carry no
 * address and stay untrapped.
 *
 * DNS: the guest's one resolver is the virtual 192.168.31.1:53. Queries to
 * it leave for the first IPv4 address in <store>/dns, which the app
 * rewrites when the phone's network or VPN changes, and replies from
 * that server come back as 192.168.31.1: the guest resolver checks the
 * source. No file (or no IPv4 in it) means 8.8.8.8. The guest never sees
 * its resolver change, so its cache is never flushed: replies' TTLs are
 * capped at TAWCROOT_DNS_TTL_CAP for answers from the old network (a
 * censored lookup before a VPN came up) to age out quickly. */
#include <stdint.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include "ports.h"
#include "errno_neg.h"
#include "fdtab.h"
#include "linkstore.h"
#include "path_scratch.h"
#include "filter.h"
#include "io.h"
#include "namespace.h"
#include "netemu.h"
#include "blocking.h"
#include "raw_sys.h"
#include "sysnr.h"
#include "tawc_string.h"
#include "usercopy.h"

#define INET 2
#define INET6 10
#define DGRAM 2
#define CLOEXEC 02000000

/* struct msghdr / mmsghdr, 64-bit. */
struct mhdr {
    uint64_t name;
    uint32_t namelen, pad;
    uint64_t iov, iovlen, control, controllen;
    int32_t flags;
    uint32_t pad2;
};

static int session(void)
{
    return (tawcroot_namespace.session & TAWC_NS_INIT_SESSION) != 0;
}

static int loopback(const unsigned char *sa, long len)
{
    if (sa[0] == INET && len >= 8) return sa[4] == 127;
    if (len < 24) return 0;
    static const unsigned char v4mapped[12] = { 0,0,0,0, 0,0,0,0, 0,0,0xff,0xff };
    const unsigned char *a = sa + 8;
    if (!memcmp(a, v4mapped, 12)) return a[12] == 127;
    for (int i = 0; i < 15; ++i) if (a[i]) return 0;
    return a[15] == 1;
}

static const unsigned char vdns[4] = { 192, 168, 31, 1 };

/* First dotted quad in `text` (one address per line), 1 if found. */
int tawcroot_ports_parse_dns(const char *text, long n, unsigned char out[4])
{
    long i = 0;
    while (i < n) {
        unsigned char ip[4];
        int part = 0, ok = 1;
        while (ok && part < 4) {
            unsigned v = 0, digits = 0;
            while (i < n && text[i] >= '0' && text[i] <= '9' && digits < 4) v = v * 10 + (unsigned)(text[i++] - '0'), ++digits;
            if (!digits || v > 255) ok = 0;
            else ip[part++] = (unsigned char)v;
            if (ok && part < 4) {
                if (i < n && text[i] == '.') ++i;
                else ok = 0;
            }
        }
        if (ok && (i == n || text[i] == '\n' || text[i] == '\r')) {
            memcpy(out, ip, 4);
            return 1;
        }
        while (i < n && text[i] != '\n') ++i;
        ++i;
    }
    return 0;
}

static void host_dns(unsigned char out[4])
{
    static const unsigned char fallback[4] = { 8, 8, 8, 8 };
    memcpy(out, fallback, 4);
    if (!tawcroot_store_host_path_len) return;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    char *path = scratch->buf[0];
    size_t pos = 0;
    if (tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &pos, tawcroot_store_host_path) ||
        tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &pos, "/dns"))
        return;
    long fd = tawc_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) return;
    char text[256];
    long n = tawc_read((int)fd, text, sizeof text);
    tawc_close((int)fd);
    if (n > 0) (void)tawcroot_ports_parse_dns(text, n, out);
}

/* Skip a (possibly compressed) DNS name at msg[*off]; 0 if malformed. */
static int skip_name(const unsigned char *msg, long len, long *off)
{
    long i = *off;
    for (int labels = 0; labels < 128; ++labels) {
        if (i >= len) return 0;
        unsigned c = msg[i];
        if (c == 0) { *off = i + 1; return 1; }
        if ((c & 0xc0) == 0xc0) {
            if (i + 2 > len) return 0;
            *off = i + 2;
            return 1;
        }
        if (c & 0xc0) return 0;
        i += 1 + (long)c;
    }
    return 0;
}

int tawcroot_dns_cap_ttl(unsigned char *msg, long len, uint32_t cap)
{
    if (len < 12) return 0;
    unsigned qd = (unsigned)msg[4] << 8 | msg[5];
    unsigned rr = ((unsigned)msg[6] << 8 | msg[7]) + ((unsigned)msg[8] << 8 | msg[9]) +
                  ((unsigned)msg[10] << 8 | msg[11]);
    long off = 12;
    int changed = 0;
    for (unsigned q = 0; q < qd; ++q) {
        if (!skip_name(msg, len, &off) || off + 4 > len) return changed;
        off += 4;
    }
    for (unsigned r = 0; r < rr; ++r) {
        if (!skip_name(msg, len, &off) || off + 10 > len) return changed;
        unsigned type = (unsigned)msg[off] << 8 | msg[off + 1];
        unsigned char *t = msg + off + 4;
        uint32_t ttl = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
        if (type != 41 /* OPT: the field is EDNS flags */ && ttl > cap) {
            t[0] = (unsigned char)(cap >> 24); t[1] = (unsigned char)(cap >> 16);
            t[2] = (unsigned char)(cap >> 8); t[3] = (unsigned char)cap;
            changed = 1;
        }
        off += 10 + ((long)msg[off + 8] << 8 | msg[off + 9]);
    }
    return changed;
}

static int is_vdns(const unsigned char *sa, long len)
{
    return len >= 8 && sa[0] == INET && sa[1] == 0 && sa[2] == 0 && sa[3] == 53 && !memcmp(sa + 4, vdns, 4);
}

int tawcroot_ports_rewrite(void *p, long len, int dir)
{
    unsigned char *sa = p;
    if (!session() || len < 4 || sa[1] != 0 || (sa[0] != INET && sa[0] != INET6)) return 0;
    unsigned port = (unsigned)sa[2] << 8 | sa[3];
    if (sa[0] == INET && len >= 8 && port == 53 && (dir == TAWC_PORTS_TO || dir == TAWC_PORTS_PEER)) {
        unsigned char host[4];
        if (dir == TAWC_PORTS_TO && !memcmp(sa + 4, vdns, 4)) {
            host_dns(sa + 4);
            return 1;
        }
        if (dir == TAWC_PORTS_PEER) {
            host_dns(host);
            if (!memcmp(sa + 4, host, 4)) {
                memcpy(sa + 4, vdns, 4);
                return 1;
            }
        }
    }
    if (dir == TAWC_PORTS_BIND || dir == TAWC_PORTS_TO) {
        if (!port || port >= 1024 || (dir == TAWC_PORTS_TO && !loopback(sa, len))) return 0;
        port += TAWCROOT_PORT_SHIFT;
    } else {
        if (port <= TAWCROOT_PORT_SHIFT || port >= TAWCROOT_PORT_SHIFT + 1024 ||
            (dir == TAWC_PORTS_PEER && !loopback(sa, len))) return 0;
        port -= TAWCROOT_PORT_SHIFT;
    }
    sa[2] = (unsigned char)(port >> 8);
    sa[3] = (unsigned char)port;
    return 1;
}

/* Copy a guest sockaddr and shift it; 1 if shifted. */
static long fetch(unsigned char *buf, uint64_t gaddr, long len, int dir)
{
    if (!gaddr || len < 4 || len > 128) return 0;
    if (tawc_copy_from_guest(buf, (size_t)len, (const void *)(uintptr_t)gaddr) < 0) return 0;
    return tawcroot_ports_rewrite(buf, len, dir);
}

long tawcroot_ports_call(const tawcroot_syscall_args *a)
{
    if (!session()) return TAWC_ENOSYS;
    unsigned char sa[128];
    switch (a->nr) {
    case TAWC_SYS_bind:
        if (!fetch(sa, (uint64_t)a->b, a->c, TAWC_PORTS_BIND)) return TAWC_ENOSYS;
        return TAWC_RAW(a->nr, a->a, (long)sa, a->c, 0, 0, 0);
    case TAWC_SYS_connect:
        if (!fetch(sa, (uint64_t)a->b, a->c, TAWC_PORTS_TO)) return TAWC_ENOSYS;
        return TAWC_RAW(a->nr, a->a, (long)sa, a->c, 0, 0, 0);
    case TAWC_SYS_sendto:
        if (!fetch(sa, (uint64_t)a->e, a->f, TAWC_PORTS_TO)) return TAWC_ENOSYS;
        return TAWC_RAW(a->nr, a->a, a->b, a->c, a->d, (long)sa, a->f);
    case TAWC_SYS_sendmsg: {
        struct mhdr mh;
        if (!a->b || tawc_copy_from_guest(&mh, sizeof mh, (const void *)(uintptr_t)a->b) < 0 ||
            !fetch(sa, mh.name, mh.namelen, TAWC_PORTS_TO))
            return TAWC_ENOSYS;
        mh.name = (uint64_t)(uintptr_t)sa;
        return TAWC_RAW(a->nr, a->a, (long)&mh, a->c, 0, 0, 0);
    }
    }
    return TAWC_ENOSYS;
}

long tawcroot_ports_socket(long fd, long domain, long type)
{
    if (fd < 0 || !session() || (domain != INET && domain != INET6) || (type & 0xf) != DGRAM)
        return fd;
    if (!(tawcroot_namespace.session & TAWC_NS_RECV_TRAP)) {
        if (tawcroot_install_fd_floor_filter(TAWC_SYS_recvfrom, 0) ||
            tawcroot_install_fd_floor_filter(TAWC_SYS_recvmsg, 0) ||
            tawcroot_install_fd_floor_filter(__NR_recvmmsg, 0))
            return fd;
        tawcroot_namespace.session |= TAWC_NS_RECV_TRAP;
    }
    long hi = tawc_fcntl((int)fd, (type & CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD,
                         TAWCROOT_RESERVED_FD_BASE);
    if (hi < 0) return fd;
    tawc_close((int)fd);
    return hi;
}

/* Unshift a source address the kernel wrote into guest memory; 1 if it
 * is now the virtual DNS server. */
static int unshift(uint64_t gaddr, uint64_t glenp, uint32_t len)
{
    unsigned char sa[128];
    if (!gaddr || len < 4) return 0;
    if (glenp) {
        uint32_t got;
        if (tawc_copy_from_guest(&got, sizeof got, (const void *)(uintptr_t)glenp) < 0) return 0;
        if (got < len) len = got;
    }
    if (len > sizeof sa) len = sizeof sa;
    if (tawc_copy_from_guest(sa, len, (const void *)(uintptr_t)gaddr) < 0) return 0;
    /* family, port and, for the DNS rewrite, the IPv4 address */
    if (!tawcroot_ports_rewrite(sa, len, TAWC_PORTS_PEER)) return 0;
    (void)tawc_copy_to_guest((void *)(uintptr_t)gaddr, sa, len >= 8 ? 8 : 4);
    return is_vdns(sa, len);
}

/* Cap the TTLs of a DNS reply the kernel wrote into guest memory. */
static void cap_reply(uint64_t gbuf, long n)
{
    if (!gbuf || n < 12) return;
    TAWCROOT_PATH_SCRATCH_AUTO(scratch);
    unsigned char *msg = (unsigned char *)scratch->buf[0];
    if (n > TAWCROOT_PATH_SCRATCH_SIZE) n = TAWCROOT_PATH_SCRATCH_SIZE;
    if (tawc_copy_from_guest(msg, (size_t)n, (const void *)(uintptr_t)gbuf) < 0) return;
    if (tawcroot_dns_cap_ttl(msg, n, TAWCROOT_DNS_TTL_CAP))
        (void)tawc_copy_to_guest((void *)(uintptr_t)gbuf, msg, (size_t)n);
}

static void unshift_msg(uint64_t gmsg, long n)
{
    struct mhdr mh;
    if (tawc_copy_from_guest(&mh, sizeof mh, (const void *)(uintptr_t)gmsg) < 0) return;
    if (!unshift(mh.name, 0, mh.namelen) || n < 0 || !mh.iov || mh.iovlen < 1) return;
    struct { uint64_t base, len; } iov;
    if (tawc_copy_from_guest(&iov, sizeof iov, (const void *)(uintptr_t)mh.iov) < 0) return;
    cap_reply(iov.base, n < (long)iov.len ? n : (long)iov.len);
}

long tawcroot_ports_recv(const tawcroot_syscall_args *a, ucontext_t *uc)
{
    if (tawcroot_fd_is_reserved((int)a->a)) return TAWC_EBADF;
    long ne = tawcroot_netemu_call(a, uc);
    if (ne != TAWC_ENOSYS) return ne;
    long flags = a->nr == TAWC_SYS_recvmsg ? a->c : a->d;
    if (!(flags & 0x40 /*MSG_DONTWAIT*/) && (ne = tawcroot_wait_fd((int)a->a, 1 /*POLLIN*/, uc))) return ne;
    long rv = TAWC_RAW(a->nr, a->a, a->b, a->c, a->d, a->e, a->f);
    if (rv < 0 || !session()) return rv;
    if (a->nr == TAWC_SYS_recvfrom) {
        if (unshift((uint64_t)a->e, (uint64_t)a->f, 128)) cap_reply((uint64_t)a->b, rv);
    } else if (a->nr == TAWC_SYS_recvmsg) {
        if (a->b) unshift_msg((uint64_t)a->b, rv);
    } else if (a->b) {
        /* recvmmsg: each message's length follows its msghdr */
        for (long i = 0; i < rv; ++i) {
            const uint64_t m = (uint64_t)a->b + (uint64_t)i * (sizeof(struct mhdr) + 8);
            uint32_t got = 0;
            (void)tawc_copy_from_guest(&got, sizeof got, (const void *)(uintptr_t)(m + sizeof(struct mhdr)));
            unshift_msg(m, (long)got);
        }
    }
    return rv;
}
