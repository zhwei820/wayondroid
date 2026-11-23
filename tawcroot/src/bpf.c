/* See include/bpf_emul.h. */

#include <stddef.h>
#include <stdint.h>

#include <asm/unistd.h>
#include <fcntl.h>
#include <linux/bpf.h>

#include "bpf_emul.h"
#include "dispatch.h"
#include "errno_neg.h"
#include "io.h"
#include "raw_sys.h"
#include "tawc_string.h"
#include "usercopy.h"

#define MFD_CLOEXEC_ 1U
#define PREFIX "tawcroot-bpfmap:"

/* Android 13 (T) netd maps, bpf_shared.h, pinned as
 * /sys/fs/bpf/netd_shared/map_netd_<name> (or /sys/fs/bpf/ on older
 * layouts). Sizes are the C structs the framework's BpfMap<K, V> mirror:
 * a mismatch makes the open fail with EBADFD, as before this emulation.
 *
 * Only the maps Java opens (NetworkStatsService, BpfInterfaceMapUpdater).
 * The native TrafficController also needs configuration_map, uid_owner_map
 * and uid_permission_map: leaving those out keeps its initMaps() failing
 * early, as without any emulation. Past initMaps it subscribes to a
 * sock_diag listener an app cannot create, through a null pointer, and
 * system_server dies with SIGSEGV on every boot. */
static const struct { const char *name; uint32_t key, value; } k_maps[] = {
	{ "map_netd_cookie_tag_map",       8,  8 },  /* cookie -> {uid, tag} */
	{ "map_netd_uid_counterset_map",   4,  1 },
	{ "map_netd_app_uid_stats_map",    4, 32 },  /* uid -> StatsValue */
	{ "map_netd_stats_map_A",         16, 32 },  /* StatsKey -> StatsValue */
	{ "map_netd_stats_map_B",         16, 32 },
	{ "map_netd_iface_stats_map",      4, 32 },
	{ "map_netd_iface_index_name_map", 4, 16 },  /* ifindex -> IFNAMSIZ name */
};

int tawcroot_bpf_map_sizes(const char *name, uint32_t *key_size, uint32_t *value_size)
{
	for (size_t i = 0; i < sizeof k_maps / sizeof k_maps[0]; i++) {
		if (!tawc_streq(name, k_maps[i].name)) continue;
		*key_size = k_maps[i].key;
		*value_size = k_maps[i].value;
		return 1;
	}
	return 0;
}

static uint32_t map_id(const char *name)
{
	uint32_t h = 2166136261u;
	for (; *name; name++) h = (h ^ (unsigned char)*name) * 16777619u;
	return (h & 0x7fffffff) | 1;
}

int tawcroot_bpf_memfd_name(char *out, size_t cap, const char *map_name, uint32_t key_size, uint32_t value_size)
{
	size_t pos = 0;
	if (tawc_str_append(out, cap, &pos, PREFIX) ||
	    tawc_str_append_dec(out, cap, &pos, key_size) || tawc_str_append(out, cap, &pos, ":") ||
	    tawc_str_append_dec(out, cap, &pos, value_size) || tawc_str_append(out, cap, &pos, ":") ||
	    tawc_str_append_dec(out, cap, &pos, map_id(map_name)) || tawc_str_append(out, cap, &pos, ":") ||
	    tawc_str_append(out, cap, &pos, map_name))
		return -1;
	return 0;
}

static int parse_u32(const char **p, const char *end, char stop, uint32_t *out)
{
	uint64_t v = 0;
	const char *s = *p;
	while (s < end && *s >= '0' && *s <= '9' && v <= 0xffffffffu) v = v * 10 + (uint64_t)(*s++ - '0');
	if (s == *p || s >= end || *s != stop || v > 0xffffffffu) return 0;
	*out = (uint32_t)v;
	*p = s + 1;
	return 1;
}

int tawcroot_bpf_parse_link(const char *link, size_t len, uint32_t *key_size, uint32_t *value_size, uint32_t *id)
{
	static const char memfd[] = "/memfd:" PREFIX;
	size_t n = sizeof memfd - 1;
	if (len <= n) return 0;
	for (size_t i = 0; i < n; i++)
		if (link[i] != memfd[i]) return 0;
	const char *p = link + n, *end = link + len;
	return parse_u32(&p, end, ':', key_size) && parse_u32(&p, end, ':', value_size) && parse_u32(&p, end, ':', id);
}

/* A stand-in map's sizes and id by fd; 0 for any other fd. */
static int stand_in(uint32_t fd, uint32_t *key_size, uint32_t *value_size, uint32_t *id)
{
	char path[40], link[256];
	size_t pos = 0;
	if (fd > 0x7fffffff || tawc_str_append(path, sizeof path, &pos, "/proc/self/fd/") ||
	    tawc_str_append_dec(path, sizeof path, &pos, (long)fd))
		return 0;
	long n = tawc_readlinkat(AT_FDCWD, path, link, sizeof link);
	return n > 0 && tawcroot_bpf_parse_link(link, (size_t)n, key_size, value_size, id);
}

static long passthrough(const tawcroot_syscall_args *a)
{
	return TAWC_RAW(a->nr, a->a, a->b, a->c, 0, 0, 0);
}

static long handle_bpf(const tawcroot_syscall_args *a, ucontext_t *uc)
{
	(void)uc;
	const long cmd = a->a;
	const uint64_t attr = (uint64_t)a->b;
	const uint64_t size = (uint64_t)(unsigned long)a->c;
	uint32_t key_size, value_size, id;
	if (!attr) return passthrough(a);

	if (cmd == BPF_OBJ_GET) {
		uint64_t pathname = 0;
		char path[256];
		if (size < sizeof pathname ||
		    tawc_copy_from_guest(&pathname, sizeof pathname, (const void *)(uintptr_t)attr) ||
		    tawc_copy_string_from_guest(path, sizeof path, (const void *)(uintptr_t)pathname) < 0)
			return passthrough(a);
		const char *name = path;
		for (const char *p = path; *p; p++)
			if (*p == '/') name = p + 1;
		char memfd[128];
		if (!tawcroot_bpf_map_sizes(name, &key_size, &value_size) ||
		    tawcroot_bpf_memfd_name(memfd, sizeof memfd, name, key_size, value_size))
			return passthrough(a);
		/* bpf() fds are always close-on-exec */
		return tawc_memfd_create(memfd, MFD_CLOEXEC_);
	}

	if (cmd == BPF_MAP_LOOKUP_ELEM || cmd == BPF_MAP_UPDATE_ELEM || cmd == BPF_MAP_DELETE_ELEM ||
	    cmd == BPF_MAP_GET_NEXT_KEY) {
		uint32_t map_fd;
		if (size < sizeof map_fd || tawc_copy_from_guest(&map_fd, sizeof map_fd, (const void *)(uintptr_t)attr) ||
		    !stand_in(map_fd, &key_size, &value_size, &id))
			return passthrough(a);
		/* Empty, and writes vanish: nothing in the guest acts on these maps */
		return cmd == BPF_MAP_UPDATE_ELEM ? 0 : TAWC_ENOENT;
	}

	if (cmd == BPF_OBJ_GET_INFO_BY_FD) {
		struct { uint32_t bpf_fd, info_len; uint64_t info; } q;
		if (size < sizeof q || tawc_copy_from_guest(&q, sizeof q, (const void *)(uintptr_t)attr) ||
		    !stand_in(q.bpf_fd, &key_size, &value_size, &id))
			return passthrough(a);
		struct bpf_map_info info;
		memset(&info, 0, sizeof info);
		info.type = BPF_MAP_TYPE_HASH;
		info.id = id;
		info.key_size = key_size;
		info.value_size = value_size;
		info.max_entries = 1;
		uint32_t n = q.info_len < sizeof info ? q.info_len : (uint32_t)sizeof info;
		if (tawc_copy_to_guest((void *)(uintptr_t)q.info, &info, n) ||
		    tawc_copy_to_guest((void *)(uintptr_t)(attr + 4), &n, sizeof n))
			return TAWC_EFAULT;
		return 0;
	}

	return passthrough(a);
}

void tawcroot_bpf_register(void)
{
	tawcroot_dispatch_install(__NR_bpf, handle_bpf);
}
