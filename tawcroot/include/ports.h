/* Privileged ports for `--init` sessions (docs/tawcroot/systemd.md). */
#pragma once
#include <stdint.h>
#include "dispatch.h"

/* Guest port p < 1024 lives on host port p + this. */
#define TAWCROOT_PORT_SHIFT 20000

enum { TAWC_PORTS_BIND, TAWC_PORTS_TO, TAWC_PORTS_LOCAL, TAWC_PORTS_PEER };

/* Rewrite an AF_INET/AF_INET6 sockaddr in place: guest→host for BIND
 * (any address) and TO (loopback destinations), host→guest for LOCAL
 * (getsockname) and PEER (loopback sources). 1 if it changed. Also
 * redirects the guest's virtual DNS server, 10.0.2.3:53 (see ports.c). */
int tawcroot_ports_rewrite(void *sa, long len, int dir);

/* Seconds the guest may cache an answer from its virtual DNS server. */
#define TAWCROOT_DNS_TTL_CAP 60

/* Lower every resource record's TTL in a DNS message to at most `cap`
 * (EDNS OPT records excepted); stops at the first malformed field.
 * 1 if anything changed. */
int tawcroot_dns_cap_ttl(unsigned char *msg, long len, uint32_t cap);

/* The first IPv4 address in a <store>/dns text, 1 if there is one. */
int tawcroot_ports_parse_dns(const char *text, long n, unsigned char out[4]);

/* bind/connect/sendto/sendmsg with a shifted address, or TAWC_ENOSYS
 * when nothing needs shifting. */
long tawcroot_ports_call(const tawcroot_syscall_args *a);

/* socket(): an inet datagram socket moves above the reserved fd floor,
 * where recvfrom/recvmsg/recvmmsg trap so reply sources unshift. */
long tawcroot_ports_socket(long fd, long domain, long type);

/* recvfrom/recvmsg/recvmmsg on fds >= the floor (lazy). */
long tawcroot_ports_recv(const tawcroot_syscall_args *a, ucontext_t *uc);
