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
