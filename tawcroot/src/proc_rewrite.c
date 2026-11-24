/* /proc/self/maps reverse-translator (pure).
 *
 * Lives in PROD_C_FOR_TESTS so the cleat unit-test orchestrator can
 * call into it directly under hosted glibc. No syscalls; the only
 * global state touched is the path_scratch pool (the per-line
 * translation buffer must not live on the handler stack). The caller
 * hands in a context with the rootfs/bind tables. The syscall-side
 * glue (open the kernel file, allocate buffers, write a memfd) lives
 * in syscalls_fs.c.
 *
 * Match policy mirrors `route_through_binds` in path_orchestrate.c:
 * longest prefix wins, with a component-boundary check on the byte
 * after the prefix so a sibling whose path happens to share the
 * prefix bytes (e.g. rootfs="/foo/rfs" vs. mapping path
 * "/foo/rfs-evil/x") doesn't match. Bind takes precedence over rootfs
 * because a bind is an authoritative replacement for that subtree.
 */

#include <stddef.h>
#include "app_hidden.h"
#include "errno_neg.h"
#include "io.h"
#include "path_scratch.h"
#include "proc_rewrite.h"
#include "tawc_string.h"

/* Test whether `host_path[0..host_len)` starts with `prefix[0..pl)` and
 * the byte after the prefix is either past-the-end or '/'. Returns the
 * length of the prefix on match, 0 on no match. */
static size_t match_host_prefix(const char *host_path, size_t host_len,
				const char *prefix, size_t pl)
{
	if (pl == 0 || pl > host_len) return 0;
	if (memcmp(host_path, prefix, pl) != 0) return 0;
	if (host_len > pl && host_path[pl] != '/') return 0;
	return pl;
}

/* Build "/<dst>/<rest>" into `out`. `dst` and `rest` are bare (no
 * leading slashes). The separator slash is suppressed when either side
 * is empty so we don't emit "//" for the rootfs case (dst empty) or a
 * trailing "/" for an exact-bind-src match (rest empty). Returns bytes
 * written or -ENAMETOOLONG. */
static long emit(char *out, size_t out_cap,
		 const char *dst, size_t dst_len,
		 const char *rest, size_t rest_len)
{
	int    need_sep = (dst_len > 0 && rest_len > 0);
	size_t need     = 1 + dst_len + (need_sep ? 1 : 0) + rest_len;
	if (need + 1 > out_cap) return TAWC_ENAMETOOLONG;

	size_t off = 0;
	out[off++] = '/';
	for (size_t i = 0; i < dst_len; i++) out[off++] = dst[i];
	if (need_sep) out[off++] = '/';
	for (size_t i = 0; i < rest_len; i++) out[off++] = rest[i];
	out[off] = 0;
	return (long)off;
}

long tawcroot_proc_reverse_translate_path(
	const tawcroot_proc_rewrite_ctx *ctx,
	const char *host_path, size_t host_len,
	char *out, size_t out_cap)
{
	if (!ctx || !host_path || !out || out_cap == 0) return TAWC_ENOENT;

	/* Bind first, longest-prefix-match. Bind src paths are authoritative
	 * replacements for that subtree of the guest view; if a host mapping
	 * lives under a bind src, the guest sees it via the bind dst, not via
	 * any rootfs-side passthrough. */
	const struct tawcroot_bind *best = 0;
	size_t                       best_pl = 0;
	for (size_t i = 0; i < ctx->n_binds; i++) {
		const struct tawcroot_bind *b = &ctx->binds[i];
		if (!b->active) continue;
		if (b->src_len == 0) continue;
		size_t pl = match_host_prefix(host_path, host_len,
		                              b->src, b->src_len);
		if (pl == 0) continue;
		if (!best || pl > best_pl) { best = b; best_pl = pl; }
	}
	if (best) {
		size_t rest_off = best_pl;
		while (rest_off < host_len && host_path[rest_off] == '/') rest_off++;
		return emit(out, out_cap,
			    best->dst, best->dst_len,
			    host_path + rest_off, host_len - rest_off);
	}

	/* Rootfs prefix. */
	size_t rpl = match_host_prefix(host_path, host_len,
				       ctx->rootfs_host_path,
				       ctx->rootfs_host_path_len);
	if (rpl > 0) {
		size_t rest_off = rpl;
		while (rest_off < host_len && host_path[rest_off] == '/') rest_off++;
		return emit(out, out_cap, "", 0,
			    host_path + rest_off, host_len - rest_off);
	}

	return TAWC_ENOENT;
}

/* Find the start of the path field in a maps line.
 *
 * Format: addr-range perms offset dev inode <whitespace>path
 * Path may be empty, a real path starting with '/', a `[bracketed]`
 * marker, or `(deleted)` etc. The kernel's show_map_vma escapes only
 * '\n' inside the path (as '\012'); spaces appear literally — the
 * '\040' space-escaping is /proc/mounts, not maps. Once we've crossed
 * the inode/whitespace boundary, everything until '\n' is the path
 * field.
 *
 * Returns the byte offset within `line[0..len)` of the first byte of
 * the path field, or `len` if no path is present (line is shorter
 * than the 5 mandatory fields).
 *
 * Strategy: walk to the end of the inode field (5th run of
 * non-whitespace tokens), then skip whitespace.
 */
static size_t find_path_offset(const char *line, size_t len)
{
	size_t i = 0;
	int    fields = 0;
	while (i < len && line[i] != '\n') {
		/* skip leading whitespace */
		while (i < len && line[i] != '\n' &&
		       (line[i] == ' ' || line[i] == '\t')) i++;
		if (i >= len || line[i] == '\n') break;
		/* if we already have 5 fields, the next token IS the path */
		if (fields == 5) return i;
		/* consume one non-whitespace token */
		while (i < len && line[i] != '\n' &&
		       line[i] != ' ' && line[i] != '\t') i++;
		fields++;
	}
	return i;  /* end-of-line: no path field */
}

/* The kernel appends exactly one tag to a maps path: " (deleted)" for
 * unlinked files (fs/proc/task_mmu.c show_map_vma). Split it off by
 * matching that literal suffix; any other " (" sequence is part of the
 * path itself. A file whose real name ends in " (deleted)" is
 * indistinguishable from the kernel tag — we split it anyway, same as
 * every other maps consumer. Returns the path length with the tag (if
 * present) excluded. */
static size_t split_deleted_tag(const char *path, size_t len)
{
	static const char tag[]   = " (deleted)";
	const size_t      tag_len = sizeof tag - 1;
	if (len >= tag_len &&
	    memcmp(path + len - tag_len, tag, tag_len) == 0)
		return len - tag_len;
	return len;
}

/* Write `n` bytes; returns -ENOSPC if buffer would overflow. */
static long out_write(char *out, size_t out_cap, size_t *off,
		      const char *src, size_t n)
{
	if (*off + n > out_cap) return TAWC_ENOSPC;
	for (size_t i = 0; i < n; i++) out[*off + i] = src[i];
	*off += n;
	return 0;
}

/* The engine's own mappings (libtawcroot.so from the host app's lib
 * dir, its store files, memfds and fastpath stubs): an app would read
 * them as a foreign library injected into its process. Only for paths
 * outside the guest's view: the whole rootfs lives under the app's data
 * dir, and hiding its files would hide the guest's own libc and APKs
 * (security SDKs then call the app tampered with). */
/* Shared memory of ours that a guest process maps, shown as what a
 * phone's process maps in its place: binder's buffer (our binder is
 * memfd-backed) and the GPU's ring buffers (gfxstream's guest driver
 * names its memfds), a Qualcomm GPU's device. Without " (deleted)":
 * these are device nodes. */
static const char *maps_alias(const char *p, size_t n)
{
	static const char *const alias[][2] = {
		{ "/memfd:tawc-binder", "/dev/binderfs/binder" },
		{ "/memfd:gfxstream", "/dev/kgsl-3d0" },
		/* ANGLE's GLES library, which the EGL library the guest loads
		 * as the Adreno one needs (rootless_prep.cpp): an Adreno
		 * driver's shader compiler, hardlinked there */
		{ "/vendor/lib64/egl/libGLESv2_angle.so", "/vendor/lib64/libllvm-glnext.so" },
	};
	for (size_t a = 0; a < sizeof alias / sizeof alias[0]; a++) {
		size_t k = 0;
		while (alias[a][0][k]) k++;
		size_t j = 0;
		while (j < k && j < n && p[j] == alias[a][0][j]) j++;
		if (j == k) return alias[a][1];
	}
	return 0;
}

static int maps_line_hidden(const char *p, size_t n)
{
	static const char *const marks[] = { "tawcroot", "com.hexadecinull.vineos", "libvine_" };
	for (size_t m = 0; m < sizeof marks / sizeof marks[0]; m++) {
		size_t k = 0;
		while (marks[m][k]) k++;
		for (size_t i = 0; i + k <= n; i++) {
			size_t j = 0;
			while (j < k && p[i + j] == marks[m][j]) j++;
			if (j == k) return 1;
		}
	}
	return 0;
}

long tawcroot_proc_maps_rewrite(
	const tawcroot_proc_rewrite_ctx *ctx,
	const char *in, size_t in_len,
	char *out, size_t out_cap)
{
	if (!ctx || !in || !out) return TAWC_ENOSPC;

	TAWCROOT_PATH_SCRATCH_AUTO(scratch);
	char *xlated = scratch->buf[0];

	size_t out_off = 0;
	size_t i       = 0;
	while (i < in_len) {
		/* find end of this line (inclusive of '\n' if present) */
		size_t line_end = i;
		while (line_end < in_len && in[line_end] != '\n') line_end++;
		int has_nl = (line_end < in_len);
		size_t line_len = line_end - i;
		const char *line = in + i;

		size_t path_off = find_path_offset(line, line_len);
		size_t path_len = line_len - path_off;

		/* Decide whether to attempt rewrite:
		 *   - empty path → emit verbatim
		 *   - starts with '[' → bracketed pseudo-path, emit verbatim
		 *   - starts with '/' → real path, try reverse-translate
		 *   - anything else → emit verbatim (defensive) */
		const char *p = line + path_off;
		const char *alias = maps_alias(p, path_len);
		if (alias) {
			long e = out_write(out, out_cap, &out_off, line, path_off);
			size_t k = 0;
			while (alias[k]) k++;
			if (e >= 0) e = out_write(out, out_cap, &out_off, alias, k);
			if (e >= 0 && has_nl) e = out_write(out, out_cap, &out_off, "\n", 1);
			if (e < 0) return e;
			i = has_nl ? line_end + 1 : line_end;
			continue;
		}
		int do_rewrite = (path_len > 0 && p[0] == '/');
		size_t plen = do_rewrite ? split_deleted_tag(p, path_len) : path_len;
		long   rl   = do_rewrite ? tawcroot_proc_reverse_translate_path(
			ctx, p, plen, xlated, TAWCROOT_PATH_SCRATCH_SIZE) : 0;
		int hidden = rl <= 0 && maps_line_hidden(p, path_len);
		/* An app sees no mapping of a file it cannot see (app_hidden.h),
		 * such as what it inherited from zygote */
		if (!hidden && rl > 0 && (size_t)rl < TAWCROOT_PATH_SCRATCH_SIZE) {
			xlated[rl] = 0;
			hidden = tawcroot_app_hidden(xlated);
		}
		if (hidden) {
			i = has_nl ? line_end + 1 : line_end;
			continue;
		}

		/* Emit prefix (everything up to the path field) verbatim. */
		long e = out_write(out, out_cap, &out_off, line, path_off);
		if (e < 0) return e;

		if (do_rewrite) {
			/* Reverse-translated only the path proper; the kernel
			 * " (deleted)" tag, if any, is re-attached verbatim. */
			if (rl > 0) {
				e = out_write(out, out_cap, &out_off,
					      xlated, (size_t)rl);
				if (e < 0) return e;
				/* trailing tag (e.g. " (deleted)") + rest */
				e = out_write(out, out_cap, &out_off,
					      p + plen, path_len - plen);
				if (e < 0) return e;
			} else {
				/* No prefix matched — emit the path verbatim. */
				e = out_write(out, out_cap, &out_off,
					      p, path_len);
				if (e < 0) return e;
			}
		} else {
			e = out_write(out, out_cap, &out_off, p, path_len);
			if (e < 0) return e;
		}

		if (has_nl) {
			e = out_write(out, out_cap, &out_off, "\n", 1);
			if (e < 0) return e;
			i = line_end + 1;
		} else {
			i = line_end;
		}
	}
	return (long)out_off;
}

/* Arm Ltd. cores that implement SVE. LLVM picks its target from the
 * `CPU part` line and enables SVE by core name; it does not check
 * HWCAP_SVE. */
static int arm_part_has_sve(unsigned long part)
{
	static const unsigned short parts[] = {
		0xd40, /* Neoverse V1 */
		0xd46, /* Cortex-A510 */
		0xd47, /* Cortex-A710 */
		0xd48, /* Cortex-X2 */
		0xd49, /* Neoverse N2 */
		0xd4d, /* Cortex-A715 */
		0xd4e, /* Cortex-X3 */
		0xd4f, /* Neoverse V2 */
		0xd80, /* Cortex-A520 */
		0xd81, /* Cortex-A720 */
		0xd82, /* Cortex-X4 */
		0xd84, /* Neoverse V3 */
		0xd85, /* Cortex-X925 */
		0xd87, /* Cortex-A725 */
		0xd8e, /* Neoverse N3 */
	};
	for (size_t k = 0; k < sizeof parts / sizeof parts[0]; k++)
		if (parts[k] == part) return 1;
	return 0;
}

/* Parse "<key><spaces/tabs>: 0x<hex>" at the start of `line`. Returns
 * the value, or -1 if the line is not that key. `*val_start` and
 * `*val_end` bracket the "0x<hex>" token. */
static long cpuinfo_hex_field(const char *line, size_t len, const char *key,
			      size_t *val_start, size_t *val_end)
{
	size_t i = 0;
	while (key[i]) {
		if (i >= len || line[i] != key[i]) return -1;
		i++;
	}
	while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
	if (i >= len || line[i] != ':') return -1;
	i++;
	while (i < len && line[i] == ' ') i++;
	if (i + 2 > len || line[i] != '0' || (line[i + 1] != 'x' && line[i + 1] != 'X'))
		return -1;
	*val_start = i;
	i += 2;
	unsigned long v = 0;
	size_t digits = 0;
	for (; i < len; i++, digits++) {
		char c = line[i];
		int d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else break;
		v = v * 16 + (unsigned long)d;
	}
	if (digits == 0 || digits > 8) return -1;
	*val_end = i;
	return (long)v;
}

long tawcroot_cpuinfo_hide_sve_cores(const char *in, size_t in_len,
				     char *out, size_t out_cap)
{
	if (!in || !out) return TAWC_ENOSPC;
	size_t out_off = 0;
	long implementer = -1;
	size_t i = 0;
	while (i < in_len) {
		size_t end = i;
		while (end < in_len && in[end] != '\n') end++;
		const char *line = in + i;
		size_t len = end - i;
		size_t vs, ve;
		long v = cpuinfo_hex_field(line, len, "CPU implementer", &vs, &ve);
		if (v >= 0) implementer = v;
		long part = implementer == 0x41
			? cpuinfo_hex_field(line, len, "CPU part", &vs, &ve) : -1;
		long e;
		if (part >= 0 && arm_part_has_sve((unsigned long)part)) {
			e = out_write(out, out_cap, &out_off, line, vs);
			if (!e) e = out_write(out, out_cap, &out_off,
					      TAWCROOT_CPUINFO_NO_SVE_PART,
					      sizeof TAWCROOT_CPUINFO_NO_SVE_PART - 1);
			if (!e) e = out_write(out, out_cap, &out_off,
					      line + ve, len - ve);
		} else {
			e = out_write(out, out_cap, &out_off, line, len);
		}
		if (!e && end < in_len) e = out_write(out, out_cap, &out_off, "\n", 1);
		if (e < 0) return e;
		i = end + 1;
	}
	return (long)out_off;
}
