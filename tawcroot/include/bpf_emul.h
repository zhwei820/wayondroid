/* Empty stand-ins for netd's BPF maps.
 *
 * An app may not use bpf(): bpfloader never pins the maps and every
 * BPF_OBJ_GET fails with EACCES. Android 13's system_server keeps going
 * without them, but NetworkStatsService then holds null maps and throws
 * on the first UID_REMOVED broadcast (an uninstall, some installs),
 * which kills system_server and soft-reboots the guest.
 *
 * So BPF_OBJ_GET of a netd map path the table below knows returns a memfd
 * named after the map's key and value sizes; the map commands on such an
 * fd behave like an empty map that drops writes, and BPF_OBJ_GET_INFO_BY_FD
 * reports the sizes the framework checks on open. Every other bpf() call,
 * programs included, goes to the kernel as before.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Key and value sizes of a netd map by its pinned file name (the path's
 * last component); 0 for a map we do not stand in for. */
int tawcroot_bpf_map_sizes(const char *name, uint32_t *key_size, uint32_t *value_size);

/* The memfd name for a stand-in map, into out[cap]; 0 or -1 if too small. */
int tawcroot_bpf_memfd_name(char *out, size_t cap, const char *map_name, uint32_t key_size, uint32_t value_size);

/* Parse a /proc/self/fd link target back: 1 and the sizes when `link`
 * names a stand-in map's memfd, else 0. */
int tawcroot_bpf_parse_link(const char *link, size_t len, uint32_t *key_size, uint32_t *value_size, uint32_t *id);

/* Install the bpf() handler. */
void tawcroot_bpf_register(void);
