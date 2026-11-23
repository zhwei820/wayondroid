/* Unit tests for the netd BPF map stand-ins (tawcroot/src/bpf.c): the
 * size table and the memfd name that carries a map's identity across
 * fork, exec and fd passing. */

#include <cleat/test.h>
#include <stdint.h>
#include <string.h>

#include "bpf_emul.h"

/* NetworkStatsService opens these; a size mismatch fails the open. */
test(bpf_known_netd_maps)
{
	uint32_t k = 0, v = 0;
	test_int_eq(tawcroot_bpf_map_sizes("map_netd_cookie_tag_map", &k, &v), 1);
	test_int_eq((long)k, 8L);
	test_int_eq((long)v, 8L);
	test_int_eq(tawcroot_bpf_map_sizes("map_netd_stats_map_A", &k, &v), 1);
	test_int_eq((long)k, 16L);
	test_int_eq((long)v, 32L);
	test_int_eq(tawcroot_bpf_map_sizes("map_netd_uid_counterset_map", &k, &v), 1);
	test_int_eq((long)v, 1L);
}

/* Programs and maps outside the table keep going to the kernel. */
test(bpf_unknown_names_are_not_emulated)
{
	uint32_t k = 7, v = 7;
	test_int_eq(tawcroot_bpf_map_sizes("prog_netd_cgroupskb_ingress_stats", &k, &v), 0);
	test_int_eq(tawcroot_bpf_map_sizes("map_gpu_mem_gpu_mem_total_map", &k, &v), 0);
	test_int_eq(tawcroot_bpf_map_sizes("map_netd_cookie_tag_map_extra", &k, &v), 0);
	/* TrafficController's own: see k_maps in bpf.c */
	test_int_eq(tawcroot_bpf_map_sizes("map_netd_configuration_map", &k, &v), 0);
	test_int_eq(tawcroot_bpf_map_sizes("map_netd_uid_owner_map", &k, &v), 0);
	test_int_eq(tawcroot_bpf_map_sizes("map_netd_uid_permission_map", &k, &v), 0);
	test_int_eq(tawcroot_bpf_map_sizes("", &k, &v), 0);
	test_int_eq((long)k, 7L);
}

/* The memfd name, as /proc/self/fd shows it, parses back to the sizes. */
test(bpf_memfd_name_round_trip)
{
	char name[128], link[160];
	test_int_eq(tawcroot_bpf_memfd_name(name, sizeof name, "map_netd_iface_stats_map", 4, 32), 0);
	strcpy(link, "/memfd:");
	strcat(link, name);
	strcat(link, " (deleted)");
	uint32_t k = 0, v = 0, id = 0;
	test_int_eq(tawcroot_bpf_parse_link(link, strlen(link), &k, &v, &id), 1);
	test_int_eq((long)k, 4L);
	test_int_eq((long)v, 32L);
	test_true(id != 0);

	/* Same map, same id; another map, another id */
	char other[128];
	uint32_t id2 = 0;
	test_int_eq(tawcroot_bpf_memfd_name(other, sizeof other, "map_netd_stats_map_B", 16, 32), 0);
	strcpy(link, "/memfd:");
	strcat(link, other);
	test_int_eq(tawcroot_bpf_parse_link(link, strlen(link), &k, &v, &id2), 1);
	test_true(id2 != id);
}

test(bpf_other_links_are_not_stand_ins)
{
	uint32_t k, v, id;
	const char *links[] = {
		"/memfd:tawcroot-maps (deleted)",
		"/memfd:tawcroot-bpfmap:",
		"/memfd:tawcroot-bpfmap:4:x:1:m",
		"/memfd:tawcroot-bpfmap:99999999999:4:1:m",
		"/data/local/tmp/tawcroot-bpfmap:4:4:1:m",
		"anon_inode:bpf-map",
	};
	for (size_t i = 0; i < sizeof links / sizeof links[0]; i++)
		test_int_eq(tawcroot_bpf_parse_link(links[i], strlen(links[i]), &k, &v, &id), 0);
}

test(bpf_memfd_name_too_small)
{
	char name[16];
	test_int_eq(tawcroot_bpf_memfd_name(name, sizeof name, "map_netd_uid_owner_map", 4, 8), -1);
}
