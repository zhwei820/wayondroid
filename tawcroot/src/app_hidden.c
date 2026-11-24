/* See include/app_hidden.h. */
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "app_hidden.h"
#include "identity.h"
#include "io.h"
#include "linkstore.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "tawc_string.h"

#define LIST_MAX (64 * 1024)

/* The list, NUL-separated and NUL-terminated, read once per process:
 * (char *)1 until read, 0 when there is none */
static char *g_list = (char *)1;

int tawcroot_guest_is_app(void)
{
	return tawcroot_identity_euid() % 100000 >= 10000;
}

static const char *list(void)
{
	char *l = __atomic_load_n(&g_list, __ATOMIC_ACQUIRE);
	if (l != (char *)1) return l;
	char *loaded = 0;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	size_t n = 0;
	if (tawcroot_store_host_path_len &&
	    !tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &n, tawcroot_store_host_path) &&
	    !tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &n, "/app-hidden")) {
		long fd = tawc_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
		long p = fd < 0 ? -1 : tawc_mmap(0, LIST_MAX + 2, PROT_READ | PROT_WRITE,
						  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (fd >= 0 && !(p < 0 && p >= -4095)) {
			char *b = (char *)p;
			long got = 0, r;
			while (got < LIST_MAX && (r = tawc_read((int)fd, b + got, LIST_MAX - got)) > 0) got += r;
			for (long i = 0; i < got; i++)
				if (b[i] == '\n') b[i] = 0;
			b[got] = b[got + 1] = 0;
			if (got > 0) loaded = b;
		}
		if (fd >= 0) tawc_close((int)fd);
	}
	/* A racing thread's copy is equal; the loser's mapping leaks, once */
	__atomic_store_n(&g_list, loaded, __ATOMIC_RELEASE);
	return loaded;
}

/* `path` is `entry` or lies under it */
static int covers(const char *entry, const char *path)
{
	size_t i = 0;
	while (entry[i] && entry[i] == path[i]) i++;
	return !entry[i] && (!path[i] || path[i] == '/');
}

int tawcroot_app_hidden(const char *path)
{
	if (!path || path[0] != '/' || !tawcroot_guest_is_app()) return 0;
	const char *l = list();
	for (; l && *l; l += tawc_strlen(l) + 1)
		if (covers(l, path)) return 1;
	return 0;
}

long tawcroot_app_hidden_filter(const char *dir, char *buf, long n)
{
	if (!dir || dir[0] != '/' || !tawcroot_guest_is_app()) return n;
	const char *l = list();
	if (!l) return n;
	size_t dl = tawc_strlen(dir);
	while (dl > 1 && dir[dl - 1] == '/') dl--;
	/* Only directories that hold a listed path */
	int any = 0;
	for (const char *e = l; *e && !any; e += tawc_strlen(e) + 1) {
		size_t i = 0;
		while (i < dl && e[i] == dir[i]) i++;
		any = i == dl && (dl == 1 || e[dl] == '/');
	}
	if (!any) return n;
	long out = 0;
	for (long off = 0; off < n;) {
		unsigned short reclen;
		__builtin_memcpy(&reclen, buf + off + 16, sizeof reclen);
		if (reclen == 0 || off + reclen > n) break;
		const char *name = buf + off + 19;
		int hide = 0;
		for (const char *e = l; *e && !hide; e += tawc_strlen(e) + 1) {
			size_t i = 0;
			while (i < dl && e[i] == dir[i]) i++;
			if (i != dl) continue;
			const char *rest = e + dl + (dl == 1 ? 0 : 1);
			if (dl != 1 && e[dl] != '/') continue;
			size_t k = 0;
			while (name[k] && rest[k] == name[k]) k++;
			hide = !name[k] && !rest[k];
		}
		if (!hide) {
			if (out != off) __builtin_memmove(buf + out, buf + off, reclen);
			out += reclen;
		}
		off += reclen;
	}
	return out;
}
