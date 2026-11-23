/* Unit tests for tawcroot_ports_parse_dns (tawcroot/src/ports.c): the
 * <store>/dns file the app writes, one resolver address per line. */

#include <cleat/test.h>
#include <string.h>

#include "ports.h"

static int parse(const char *text, unsigned char out[4])
{
	return tawcroot_ports_parse_dns(text, (long)strlen(text), out);
}

test(dns_first_ipv4_line)
{
	unsigned char ip[4] = { 0 };
	test_int_eq(parse("192.168.5.1\n114.114.114.114\n", ip), 1);
	test_int_eq(ip[0], 192);
	test_int_eq(ip[1], 168);
	test_int_eq(ip[2], 5);
	test_int_eq(ip[3], 1);
}

/* A VPN's resolver, no trailing newline. */
test(dns_single_line_without_newline)
{
	unsigned char ip[4] = { 0 };
	test_int_eq(parse("198.18.0.2", ip), 1);
	test_int_eq(ip[0], 198);
	test_int_eq(ip[3], 2);
}

/* IPv6 lines are skipped: the guest's resolver socket is AF_INET. */
test(dns_skips_ipv6_lines)
{
	unsigned char ip[4] = { 0 };
	test_int_eq(parse("2408:8001:7000::1\n10.0.0.53\n", ip), 1);
	test_int_eq(ip[0], 10);
	test_int_eq(ip[3], 53);
}

test(dns_rejects_malformed_addresses)
{
	unsigned char ip[4] = { 9, 9, 9, 9 };
	test_int_eq(parse("256.1.1.1\n1.2.3\n1.2.3.4x\n2408::1\n", ip), 0);
	test_int_eq(ip[0], 9);
}

test(dns_empty_file)
{
	unsigned char ip[4] = { 9, 9, 9, 9 };
	test_int_eq(parse("", ip), 0);
	test_int_eq(ip[0], 9);
}

/* --- TTL cap on replies from the virtual resolver ---------------- */

/* www.google.com A: two answers (300 s, 30 s) and an EDNS OPT record
 * whose TTL field carries the DO flag. */
static const unsigned char reply[] = {
	0x12, 0x34, 0x81, 0x80, 0, 1, 0, 2, 0, 0, 0, 1,
	3, 'w', 'w', 'w', 6, 'g', 'o', 'o', 'g', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1,
	0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0x01, 0x2c, 0, 4, 142, 250, 1, 1,
	0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 142, 250, 1, 2,
	0, 0, 41, 0x04, 0xd0, 0, 0, 0x80, 0, 0, 0,
};

static uint32_t ttl_at(const unsigned char *m, int off)
{
	return (uint32_t)m[off] << 24 | (uint32_t)m[off + 1] << 16 | (uint32_t)m[off + 2] << 8 | m[off + 3];
}

test(dns_ttl_capped_short_and_opt_untouched)
{
	unsigned char m[sizeof reply];
	memcpy(m, reply, sizeof m);
	test_int_eq(tawcroot_dns_cap_ttl(m, (long)sizeof m, 60), 1);
	test_int_eq((long)ttl_at(m, 38), 60L);       /* 300 -> 60 */
	test_int_eq((long)ttl_at(m, 54), 30L);       /* already below */
	test_int_eq((long)ttl_at(m, 69), 0x8000L);   /* OPT flags kept */
	test_int_eq(memcmp(m + 42, reply + 42, 6), 0); /* rdata untouched */
}

/* A cut-off message changes only the records that fit and never reads
 * past the end. */
test(dns_ttl_truncated_message)
{
	unsigned char m[sizeof reply];
	memcpy(m, reply, sizeof m);
	test_int_eq(tawcroot_dns_cap_ttl(m, 50, 60), 1);
	test_int_eq((long)ttl_at(m, 38), 60L);
	test_int_eq((long)ttl_at(m, 54), 30L);
}

test(dns_ttl_garbage_is_left_alone)
{
	unsigned char m[16] = { 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0x40, 1, 2, 3 };
	unsigned char before[16];
	memcpy(before, m, sizeof m);
	test_int_eq(tawcroot_dns_cap_ttl(m, (long)sizeof m, 60), 0);
	test_int_eq(memcmp(m, before, sizeof m), 0);
	test_int_eq(tawcroot_dns_cap_ttl(m, 5, 60), 0);
}
