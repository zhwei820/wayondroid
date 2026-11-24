/* Unit tests for tawcroot_netemu_parse_mac (tawcroot/src/netemu.c): the
 * <store>/mac file the engine writes, the fake wlan0's address. */

#include <cleat/test.h>
#include <string.h>

#include "netemu.h"

static int parse(const char *text, uint8_t out[6])
{
	return tawcroot_netemu_parse_mac(text, (long)strlen(text), out);
}

test(mac_with_newline)
{
	uint8_t mac[6] = { 0 };
	test_int_eq(parse("3a:0F:c1:22:9b:e4\n", mac), 1);
	test_int_eq(mac[0], 0x3a);
	test_int_eq(mac[1], 0x0f);
	test_int_eq(mac[5], 0xe4);
}

test(mac_without_newline)
{
	uint8_t mac[6] = { 0 };
	test_int_eq(parse("02:00:00:00:00:01", mac), 1);
	test_int_eq(mac[5], 1);
}

/* Malformed or multicast: rejected, out untouched. */
test(mac_rejects_bad_text)
{
	uint8_t mac[6] = { 7, 7, 7, 7, 7, 7 };
	test_int_eq(parse("", mac), 0);
	test_int_eq(parse("3a:0f:c1:22:9b", mac), 0);
	test_int_eq(parse("3a-0f-c1-22-9b-e4", mac), 0);
	test_int_eq(parse("3a:0f:c1:22:9b:e4x", mac), 0);
	test_int_eq(parse("3a:0f:c1:22:9b:g4", mac), 0);
	test_int_eq(parse("01:00:5e:00:00:01", mac), 0);
	test_int_eq(mac[0], 7);
	test_int_eq(mac[5], 7);
}
