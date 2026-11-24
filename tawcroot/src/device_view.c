/* See include/device_view.h. */
#include <stddef.h>
#include <stdint.h>
#include <fcntl.h>
#include <asm/unistd.h>
#include "device_view.h"
#include "app_hidden.h"
#include "errno_neg.h"
#include "io.h"
#include "linkstore.h"
#include "loader_exec.h"
#include "path.h"
#include "path_scratch.h"
#include "raw_sys.h"
#include "tawc_string.h"
#include "usercopy.h"

#define FIX 20 /* fixed-point bits of a scale factor */

/* <store>/device/<name> into buf, NUL-terminated: its length, or -1 */
static long read_device(const char *name, char *buf, size_t cap)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0];
	size_t n = 0;
	if (!tawcroot_store_host_path_len || cap < 2 ||
	    tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &n, tawcroot_store_host_path) ||
	    tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &n, "/device/") ||
	    tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &n, name))
		return -1;
	long fd = tawc_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
	if (fd < 0) return -1;
	long got = 0, r = 0;
	while ((size_t)got < cap - 1 && (r = tawc_read((int)fd, buf + got, cap - 1 - (size_t)got)) > 0) got += r;
	tawc_close((int)fd);
	if (r < 0) return -1; /* a directory */
	buf[got] = 0;
	return got;
}

static unsigned long parse_ul(const char *s)
{
	unsigned long v = 0;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0');
	return v;
}

/* A number file's value, read once per process; 0 when absent */
static unsigned long device_number(const char *name, long *cache)
{
	long v = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
	if (v >= 0) return (unsigned long)v;
	char buf[32];
	v = read_device(name, buf, sizeof buf) > 0 ? (long)parse_ul(buf) : 0;
	__atomic_store_n(cache, v, __ATOMIC_RELEASE);
	return (unsigned long)v;
}

static long g_meminfo_kb = -1, g_data_bytes = -1, g_has_device = -1;

static int has_device(void)
{
	long v = __atomic_load_n(&g_has_device, __ATOMIC_ACQUIRE);
	if (v < 0) {
		char b[2];
		v = read_device("uname", b, sizeof b) >= 0;
		__atomic_store_n(&g_has_device, v, __ATOMIC_RELEASE);
	}
	return (int)v;
}

/* target/real in FIX bits, 0 when no smaller target */
static unsigned long scale_to(unsigned long target, unsigned long real)
{
	if (!target || !real || target >= real) return 0;
	return (target << FIX) / real;
}

static long memfd_of(const char *name, const char *bytes, size_t len, int flags)
{
	long fd = tawc_memfd_create(name, (flags & O_CLOEXEC) ? 1U : 0U);
	if (fd < 0) return fd;
	size_t done = 0;
	while (done < len) {
		long w = tawc_write((int)fd, bytes + done, len - done);
		if (w <= 0) {
			tawc_close((int)fd);
			return w < 0 ? w : TAWC_EIO;
		}
		done += (size_t)w;
	}
	(void)tawc_lseek((int)fd, 0, 0 /* SEEK_SET */);
	return fd;
}

/* The kernel's file in full (up to cap-1 bytes), NUL-terminated */
static long read_host(const char *path, char *buf, size_t cap)
{
	long fd = tawc_openat(AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) return fd;
	long got = 0, r;
	while ((size_t)got < cap - 1 && (r = tawc_read((int)fd, buf + got, cap - 1 - (size_t)got)) > 0) got += r;
	tawc_close((int)fd);
	buf[got] = 0;
	return got;
}

/* /proc/meminfo with every "N kB" scaled, each number kept right-aligned
 * in its field as the kernel prints it */
static long open_meminfo(int flags)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *in = scratch->buf[1], *out = scratch->buf[2];
	long n = read_host("/proc/meminfo", in, TAWCROOT_PATH_SCRATCH_SIZE);
	if (n < 0) return TAWC_ENOSYS;
	const char *mt = in;
	while (*mt && !tawc_starts_with(mt, "MemTotal:")) mt++;
	unsigned long k = *mt ? scale_to(device_number("meminfo-kb", &g_meminfo_kb), parse_ul(mt + 9)) : 0;
	if (!k) return TAWC_ENOSYS;
	size_t o = 0;
	for (const char *p = in; *p;) {
		const char *eol = p;
		while (*eol && *eol != '\n') eol++;
		const char *colon = p;
		while (colon < eol && *colon != ':') colon++;
		const char *d = colon + 1, *de;
		while (d < eol && *d == ' ') d++;
		for (de = d; de < eol && *de >= '0' && *de <= '9'; de++) {}
		size_t len = (size_t)(eol - p) + (*eol ? 1 : 0);
		if (colon < eol && de > d && eol - de == 3 && tawc_starts_with(de, " kB")) {
			char digits[24];
			int nd = 0;
			unsigned long v = (parse_ul(d) * k) >> FIX;
			do { digits[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
			size_t width = (size_t)(de - (colon + 1));
			if (o + (size_t)(colon + 1 - p) + width + 4 + (size_t)nd >= TAWCROOT_PATH_SCRATCH_SIZE) break;
			for (const char *c = p; c <= colon; c++) out[o++] = *c;
			for (size_t s = (size_t)nd; s < width; s++) out[o++] = ' ';
			while (nd) out[o++] = digits[--nd];
			for (const char *c = de; c < eol; c++) out[o++] = *c;
			if (*eol) out[o++] = '\n';
		} else {
			if (o + len >= TAWCROOT_PATH_SCRATCH_SIZE) break;
			for (size_t i = 0; i < len; i++) out[o++] = p[i];
		}
		p += len;
	}
	return memfd_of("tawcroot-meminfo", out, o, flags);
}

size_t tawcroot_device_status(char *buf, size_t len, size_t cap)
{
	if (!tawcroot_guest_is_app() || !has_device()) return len;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *out = scratch->buf[1];
	static const char *const fixed[][2] = {
		{ "Seccomp_filters:", "\t1" }, { "Cpus_allowed:", "\tff" }, { "Cpus_allowed_list:", "\t0-7" },
	};
	size_t o = 0;
	for (size_t i = 0; i < len;) {
		size_t eol = i;
		while (eol < len && buf[eol] != '\n') eol++;
		size_t next = eol < len ? eol + 1 : eol;
		const char *value = 0;
		size_t keylen = 0;
		for (size_t f = 0; f < sizeof fixed / sizeof fixed[0] && !value; f++) {
			size_t kl = tawc_strlen(fixed[f][0]), k = 0;
			while (k < kl && i + k < eol && buf[i + k] == fixed[f][0][k]) k++;
			if (k == kl) { keylen = kl; value = fixed[f][1]; }
		}
		if (o + (next - i) + 16 >= TAWCROOT_PATH_SCRATCH_SIZE) return len;
		if (value) {
			for (size_t k = 0; k < keylen; k++) out[o++] = buf[i + k];
			for (const char *v = value; *v; v++) out[o++] = *v;
			if (eol < len) out[o++] = '\n';
		} else {
			for (size_t k = i; k < next; k++) out[o++] = buf[k];
		}
		i = next;
	}
	if (o > cap) return len;
	for (size_t k = 0; k < o; k++) buf[k] = out[k];
	return o;
}

/* /proc/self/<leaf>, /proc/thread-self/<leaf> or /proc/<our pid>/<leaf> */
static int own_proc_file(const char *path, const char *leaf)
{
	if (!tawc_starts_with(path, "/proc/")) return 0;
	const char *p = path + 6;
	if (tawc_starts_with(p, "self/")) p += 5;
	else if (tawc_starts_with(p, "thread-self/")) p += 12;
	else {
		unsigned long pid = 0;
		const char *q = p;
		while (*q >= '0' && *q <= '9') pid = pid * 10 + (unsigned long)(*q++ - '0');
		if (q == p || *q != '/' || pid != (unsigned long)tawc_getpid()) return 0;
		p = q + 1;
	}
	return tawc_streq(p, leaf);
}

/* The auxv the loader gave the guest (loader_stack.c), not the kernel's
 * for tawcroot itself: that one names tawcroot's own load address and the
 * host's whole HWCAP */
static long open_auxv(int flags)
{
	const uint64_t *w = (const uint64_t *)tawcroot_guest_start_stack;
	if (!w) return TAWC_ENOSYS;
	w += 1 + w[0] + 1;          /* argc, argv, NULL */
	while (*w) w++;             /* envp */
	w++;
	const uint64_t *aux = w;
	while (w[0] != 0) w += 2;   /* to AT_NULL */
	return memfd_of("tawcroot-auxv", (const char *)aux, (size_t)((const char *)(w + 2) - (const char *)aux), flags);
}

void tawcroot_device_hwcap(uint64_t *hwcap, uint64_t *hwcap2)
{
	char text[64];
	if (read_device("hwcap", text, sizeof text) <= 0) return;
	uint64_t v[2] = { 0, 0 };
	const char *c = text;
	for (int i = 0; i < 2; i++) {
		while (*c == ' ' || *c == '\n') c++;
		if (c[0] == '0' && (c[1] == 'x' || c[1] == 'X')) c += 2;
		for (;; c++) {
			int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : -1;
			if (d < 0) break;
			v[i] = v[i] * 16 + (uint64_t)d;
		}
	}
	*hwcap &= v[0];
	*hwcap2 &= v[1];
}

/* "policy<n>" names of a getdents64 buffer or directory, ascending */
static int policy_number(const char *name)
{
	if (!tawc_starts_with(name, "policy") || !name[6]) return -1;
	int v = 0;
	for (const char *c = name + 6; *c; c++) {
		if (*c < '0' || *c > '9') return -1;
		v = v * 10 + (*c - '0');
	}
	return v;
}

static int sorted_policies(const char *buf, long n, int *out, int cap)
{
	int count = 0;
	for (long off = 0; off < n;) {
		unsigned short reclen;
		__builtin_memcpy(&reclen, buf + off + 16, sizeof reclen);
		if (reclen == 0 || off + reclen > n) break;
		int v = policy_number(buf + off + 19);
		if (v >= 0 && count < cap) {
			int i = count++;
			while (i > 0 && out[i - 1] > v) { out[i] = out[i - 1]; i--; }
			out[i] = v;
		}
		off += reclen;
	}
	return count;
}

/* `path` is a line of <store>/device/<file> (guest paths, one a line) */
static int listed_in(const char *file, const char *path)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *list = scratch->buf[1];
	if (read_device(file, list, TAWCROOT_PATH_SCRATCH_SIZE) <= 0) return 0;
	size_t pl = tawc_strlen(path);
	while (pl > 1 && path[pl - 1] == '/') pl--;
	for (const char *l = list; *l;) {
		size_t k = 0;
		while (l[k] && l[k] != '\n') k++;
		if (k == pl) {
			size_t i = 0;
			while (i < k && l[i] == path[i]) i++;
			if (i == k) return 1;
		}
		l += k + (l[k] ? 1 : 0);
	}
	return 0;
}

/* Drops the entries of `dir` that <store>/device/unlisted names */
static long drop_unlisted(const char *dir, char *buf, long n)
{
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *list = scratch->buf[1];
	if (read_device("unlisted", list, TAWCROOT_PATH_SCRATCH_SIZE) <= 0) return n;
	size_t dl = tawc_strlen(dir);
	while (dl > 1 && dir[dl - 1] == '/') dl--;
	long out = 0;
	for (long off = 0; off < n;) {
		unsigned short reclen;
		__builtin_memcpy(&reclen, buf + off + 16, sizeof reclen);
		if (reclen == 0 || off + reclen > n) break;
		const char *name = buf + off + 19;
		int drop = 0;
		for (const char *l = list; *l && !drop;) {
			size_t k = 0;
			while (l[k] && l[k] != '\n') k++;
			size_t i = 0;
			while (i < dl && i < k && l[i] == dir[i]) i++;
			if (i == dl && k > dl + 1 && l[dl] == '/') {
				size_t j = 0;
				while (name[j] && dl + 1 + j < k && name[j] == l[dl + 1 + j]) j++;
				drop = !name[j] && dl + 1 + j == k;
			}
			l += k + (l[k] ? 1 : 0);
		}
		if (!drop) {
			if (out != off) __builtin_memmove(buf + out, buf + off, reclen);
			out += reclen;
		}
		off += reclen;
	}
	return out;
}

long tawcroot_device_dirents(const char *dir, char *buf, long n)
{
	if (!has_device()) return n;
	n = drop_unlisted(dir, buf, n);
	if (!tawc_streq(dir, "/sys/devices/system/cpu/cpufreq")) return n;
	int host[16], dev[16];
	int nh = sorted_policies(buf, n, host, 16);
	if (!nh) return n;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *path = scratch->buf[0], *list = scratch->buf[1];
	size_t k = 0;
	if (tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &k, tawcroot_store_host_path) ||
	    tawc_str_append(path, TAWCROOT_PATH_SCRATCH_SIZE, &k, "/device/sys-cpu/cpufreq"))
		return n;
	long fd = tawc_openat(AT_FDCWD, path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
	if (fd < 0) return n;
	long got = tawc_getdents64((int)fd, list, TAWCROOT_PATH_SCRATCH_SIZE);
	tawc_close((int)fd);
	if (got <= 0 || sorted_policies(list, got, dev, 16) != nh) return n;
	/* The i-th of the phone's policies is the device's i-th: renamed in
	 * place, when the names are as long */
	for (long off = 0; off < n;) {
		unsigned short reclen;
		__builtin_memcpy(&reclen, buf + off + 16, sizeof reclen);
		if (reclen == 0 || off + reclen > n) break;
		char *name = buf + off + 19;
		int v = policy_number(name);
		for (int i = 0; v >= 0 && i < nh; i++) {
			if (host[i] != v) continue;
			if ((host[i] < 10) == (dev[i] < 10) && dev[i] < 100) {
				if (dev[i] >= 10) { name[6] = (char)('0' + dev[i] / 10); name[7] = (char)('0' + dev[i] % 10); }
				else name[6] = (char)('0' + dev[i]);
			}
			break;
		}
		off += reclen;
	}
	return n;
}

long tawcroot_device_open(const char *path, int flags)
{
	/* A directory an app may search but not read, as /dev under a phone's policy */
	if (!(flags & O_PATH) && tawcroot_guest_is_app() && has_device() && listed_in("unlistable", path))
		return TAWC_EACCES;
	if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_PATH | O_DIRECTORY))) return TAWC_ENOSYS;
	if (tawc_streq(path, "/proc/meminfo")) return open_meminfo(flags);
	if (own_proc_file(path, "auxv")) return open_auxv(flags);
	if (!tawcroot_guest_is_app() || !has_device()) return TAWC_ENOSYS;
	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *buf = scratch->buf[1];
	long n;
	if (tawc_streq(path, "/proc/cpuinfo")) {
		n = read_device("cpuinfo", buf, TAWCROOT_PATH_SCRATCH_SIZE);
		return n > 0 ? memfd_of("tawcroot-cpuinfo", buf, (size_t)n, flags) : TAWC_ENOSYS;
	}
	static const char cpu[] = "/sys/devices/system/cpu/";
	if (!tawc_starts_with(path, cpu)) return TAWC_ENOSYS;
	const char *rest = path + sizeof cpu - 1;
	for (const char *c = rest; *c; c++)
		if (c[0] == '.' && (c == rest || c[-1] == '/')) return TAWC_ENOSYS;
	char *name = scratch->buf[2];
	size_t k = 0;
	if (tawc_str_append(name, TAWCROOT_PATH_SCRATCH_SIZE, &k, "sys-cpu/") ||
	    tawc_str_append(name, TAWCROOT_PATH_SCRATCH_SIZE, &k, rest))
		return TAWC_ENOSYS;
	n = read_device(name, buf, TAWCROOT_PATH_SCRATCH_SIZE);
	return n >= 0 ? memfd_of("tawcroot-sys-cpu", buf, (size_t)n, flags) : TAWC_ENOSYS;
}

void tawcroot_device_statfs(long *st)
{
	if (st[0] != 0xF2F52010L && st[0] != 0xEF53L) return; /* f2fs, ext4 */
	if (st[1] <= 0 || st[2] <= 0) return;
	unsigned long target = device_number("data-bytes", &g_data_bytes) / (unsigned long)st[1];
	unsigned long k = scale_to(target, (unsigned long)st[2]);
	if (!k) return;
	st[2] = (long)target;
	for (int i = 3; i <= 6; i++) st[i] = (long)(((unsigned long)st[i] * k) >> FIX);
}

long tawcroot_device_uname(char *uts)
{
	if (!tawcroot_guest_is_app() &&
	    !(tawcroot_guest_exe_path_len && tawc_streq(tawcroot_guest_exe_path, "/system/bin/app_process64")))
		return 0;
	char text[160], field[65];
	if (read_device("uname", text, sizeof text) <= 0) return 0;
	const char *line = text;
	for (int f = 2; f <= 3 && *line; f++) {
		size_t i = 0;
		while (line[i] && line[i] != '\n' && i < 64) { field[i] = line[i]; i++; }
		for (size_t z = i; z < 65; z++) field[z] = 0;
		if (tawc_copy_to_guest(uts + f * 65, field, 65) < 0) return TAWC_EFAULT;
		while (*line && *line != '\n') line++;
		if (*line) line++;
	}
	return 0;
}

/* struct sysinfo, LP64 */
struct tawc_sysinfo {
	long uptime;
	unsigned long loads[3];
	unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
	unsigned short procs, pad;
	unsigned long totalhigh, freehigh;
	unsigned int mem_unit;
};

long tawcroot_device_sysinfo(const tawcroot_syscall_args *a, ucontext_t *uc)
{
	(void)uc;
	struct tawc_sysinfo si;
	long rv = TAWC_RAW(__NR_sysinfo, (long)&si, 0, 0, 0, 0, 0);
	if (rv) return rv;
	unsigned long unit = si.mem_unit ? si.mem_unit : 1;
	unsigned long target = device_number("meminfo-kb", &g_meminfo_kb) * 1024 / unit;
	unsigned long k = scale_to(target, si.totalram);
	if (k) {
		si.totalram = target;
		si.freeram = (si.freeram * k) >> FIX;
		si.sharedram = (si.sharedram * k) >> FIX;
		si.bufferram = (si.bufferram * k) >> FIX;
	}
	return tawc_copy_to_guest((void *)(uintptr_t)a->a, &si, sizeof si) < 0 ? TAWC_EFAULT : 0;
}
