/* Unit tests for the /proc/self/maps reverse-translator
 * (tawcroot/src/proc_rewrite.c).
 *
 * The rewriter is pure with respect to the OS: the caller passes a
 * tawcroot_proc_rewrite_ctx with the rootfs host prefix and bind table.
 * The handler-side glue (read the kernel file, write a memfd) is in
 * syscalls_fs.c and out of scope here. These tests exercise:
 *
 *   - Single-path reverse-translation: rootfs prefix, bind src,
 *     longest-prefix-match across overlapping binds, component-boundary
 *     correctness (sibling sharing prefix bytes must not match), and
 *     output-buffer overflow.
 *   - Whole-file maps rewrite: typical line, bracketed pseudo-paths
 *     pass through, anonymous (empty path) lines pass through, multi-
 *     line inputs, the "(deleted)" trailing tag is preserved, output
 *     buffer too small returns -ENOSPC, and a partial trailing line
 *     (no '\n') is emitted as-is.
 */

#include <cleat/test.h>
#include <stddef.h>
#include <string.h>

#include "path.h"
#include "proc_rewrite.h"

/* --- helpers ----------------------------------------------------- */

/* Populate a tawcroot_bind for a reverse-translation test. The orchestration
 * tests use mk_bind() with empty src; we need src filled in here. */
static void mk_bind_full(struct tawcroot_bind *b,
			 const char *src, const char *dst)
{
	size_t sn = strlen(src);
	size_t dn = strlen(dst);
	memcpy(b->src, src, sn + 1);
	memcpy(b->dst, dst, dn + 1);
	b->src_len = sn;
	b->dst_len = dn;
	b->src_fd  = 0;
	b->active  = 1;
}

/* --- single-path reverse-translation ----------------------------- */

test(rev_rootfs_prefix_strips)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs/usr/lib/libfoo.so";
	char out[256];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/usr/lib/libfoo.so"));
	test_str_eq(out, "/usr/lib/libfoo.so");
}

test(rev_rootfs_exact_match_is_root)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs";
	char out[16];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, 1);
	test_str_eq(out, "/");
}

test(rev_rootfs_with_trailing_slash_is_root)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs/";
	char out[16];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, 1);
	test_str_eq(out, "/");
}

test(rev_rootfs_sibling_prefix_does_not_match)
{
	/* "<rootfs>-evil/foo" byte-matches the rootfs prefix but is NOT
	 * inside the view — the byte after the prefix is '-', not '/' or
	 * end-of-string. The component-boundary check must reject. */
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs-evil/foo";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, -2);  /* -ENOENT */
}

test(rev_no_match_returns_enoent)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/usr/lib/libtawcroot.so";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, -2);
}

test(rev_bind_src_match)
{
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/system_real", "system");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] = "/system_real/lib/libc.so";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/system/lib/libc.so"));
	test_str_eq(out, "/system/lib/libc.so");
}

test(rev_bind_longest_prefix_wins)
{
	struct tawcroot_bind binds[2];
	mk_bind_full(&binds[0], "/host/a",   "alpha");
	mk_bind_full(&binds[1], "/host/a/b", "beta");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 2,
	};
	const char in[] = "/host/a/b/c";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/beta/c"));
	test_str_eq(out, "/beta/c");
}

test(rev_bind_sibling_prefix_does_not_match)
{
	/* "/system_ext/foo" must not match a bind src "/system". */
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/system", "system");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] = "/system_ext/foo";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, -2);
}

test(rev_bind_outranks_rootfs)
{
	/* Bind src "/data/rootfs/lib" sits inside the rootfs prefix. The
	 * mapping points there. Bind takes precedence (matches first
	 * because it's the longer prefix anyway, and conceptually because
	 * binds are authoritative). */
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/data/rootfs/lib", "lib-bound");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] = "/data/rootfs/lib/libc.so";
	char out[64];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), out, sizeof out);
	test_int_eq(n, (long)strlen("/lib-bound/libc.so"));
	test_str_eq(out, "/lib-bound/libc.so");
}

test(rev_output_overflow_returns_nametoolong)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] = "/data/rootfs/usr/lib/libfoo.so";
	char small[8];
	long n = tawcroot_proc_reverse_translate_path(
		&ctx, in, strlen(in), small, sizeof small);
	test_int_eq(n, -36);  /* -ENAMETOOLONG */
}

/* --- whole-file maps rewrite ------------------------------------- */

test(maps_rewrites_rootfs_path_in_typical_line)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f1234567000-7f1234589000 r-xp 00000000 08:01 1234567 /data/rootfs/usr/lib/libfoo.so\n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"7f1234567000-7f1234589000 r-xp 00000000 08:01 1234567 /usr/lib/libfoo.so\n");
}

test(maps_passes_bracketed_paths_through)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7ffd00000000-7ffd00021000 rw-p 00000000 00:00 0 [stack]\n"
		"7ffd00021000-7ffd00023000 r--p 00000000 00:00 0 [vvar]\n"
		"7ffd00023000-7ffd00024000 r-xp 00000000 00:00 0 [vdso]\n";
	char out[512];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n, (long)strlen(in));
	out[n] = 0;
	test_str_eq(out, in);
}

test(maps_passes_anonymous_lines_through)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	/* Anonymous mapping: empty path field after the inode. */
	const char in[] =
		"7f0000000000-7f0000010000 rw-p 00000000 00:00 0 \n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n, (long)strlen(in));
	out[n] = 0;
	test_str_eq(out, in);
}

test(maps_preserves_deleted_tag)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f1234567000-7f1234589000 r--p 00000000 08:01 999 /data/rootfs/tmp/scratch (deleted)\n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"7f1234567000-7f1234589000 r--p 00000000 08:01 999 /tmp/scratch (deleted)\n");
}

test(maps_aliases_binder_and_gpu_memfds)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f0000000000-7f0000100000 r--p 00000000 00:01 11 /memfd:tawc-binder-stage (deleted)\n"
		"7f0000100000-7f0000200000 rw-s 00000000 00:01 12 /memfd:gfxstream-ringblob-shmem-2288 (deleted)\n";
	char out[512];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"7f0000000000-7f0000100000 r--p 00000000 00:01 11 /dev/binderfs/binder\n"
		"7f0000100000-7f0000200000 rw-s 00000000 00:01 12 /dev/kgsl-3d0\n");
}

test(maps_host_prefix_containing_paren_space_still_translates)
{
	/* A rootfs host prefix containing " (" used to trip the tag-split
	 * (it split at the FIRST " ("), leaking the host path verbatim.
	 * The tag is now matched as the literal " (deleted)" suffix. */
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/app (1)/rootfs",
		.rootfs_host_path_len = 20,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"a000-b000 r-xp 00000000 08:01 1 /data/app (1)/rootfs/usr/lib/libfoo.so\n"
		"c000-d000 r--p 00000000 08:01 2 /data/app (1)/rootfs/tmp/scratch (deleted)\n";
	char out[512];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"a000-b000 r-xp 00000000 08:01 1 /usr/lib/libfoo.so\n"
		"c000-d000 r--p 00000000 08:01 2 /tmp/scratch (deleted)\n");
}

test(maps_deleted_tag_split_only_at_literal_suffix)
{
	/* " (" mid-path is path content, not a tag; only a trailing
	 * " (deleted)" is split off. */
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"a000-b000 r--p 00000000 08:01 3 /data/rootfs/x (y)/z (deleted)\n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"a000-b000 r--p 00000000 08:01 3 /x (y)/z (deleted)\n");
}

test(maps_passes_unmapped_host_paths_through)
{
	/* Paths outside the rootfs and any bind src have no guest-visible
	 * counterpart; emit them verbatim rather than dropping the line. */
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f0000000000-7f0000020000 r-xp 00000000 08:01 1 /usr/lib/libtawcroot.so\n";
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n, (long)strlen(in));
	out[n] = 0;
	test_str_eq(out, in);
}

test(maps_rewrites_each_line_independently)
{
	struct tawcroot_bind binds[1];
	mk_bind_full(&binds[0], "/system_real", "system");
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = binds,
		.n_binds              = 1,
	};
	const char in[] =
		"a000-b000 r-xp 00000000 00:00 1 /data/rootfs/usr/lib/libc.so\n"
		"c000-d000 r--p 00000000 00:00 0 [heap]\n"
		"e000-f000 r-xp 00000000 00:00 2 /system_real/lib/libdl.so\n";
	char out[512];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"a000-b000 r-xp 00000000 00:00 1 /usr/lib/libc.so\n"
		"c000-d000 r--p 00000000 00:00 0 [heap]\n"
		"e000-f000 r-xp 00000000 00:00 2 /system/lib/libdl.so\n");
}

test(maps_partial_trailing_line_still_rewritten)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"a000-b000 r--p 00000000 00:00 0 [heap]\n"
		"c000-d000 r-xp 00000000 00:00 1 /data/rootfs/usr/bin/sh";  /* no \n */
	char out[256];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"a000-b000 r--p 00000000 00:00 0 [heap]\n"
		"c000-d000 r-xp 00000000 00:00 1 /usr/bin/sh");
}

/* The rootfs sits under the app's data dir: guest files keep their
 * lines (translated); only the engine's own files outside it go. */
test(maps_hides_engine_files_but_not_the_guests)
{
	const char rootfs[] = "/data/data/com.hexadecinull.vineos/files/i/rootless/rootfs";
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = rootfs,
		.rootfs_host_path_len = sizeof rootfs - 1,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"1000-2000 r-xp 00000000 fe:49 1 /data/data/com.hexadecinull.vineos/files/i/rootless/rootfs/system/lib64/libc.so\n"
		"2000-3000 r-xp 00000000 fe:49 2 /data/app/~~x==/com.hexadecinull.vineos-y==/lib/arm64/libtawcroot.so\n"
		"3000-4000 r--p 00000000 fe:49 3 /data/data/com.hexadecinull.vineos/files/i/rootless/tawcroot/owners\n"
		"4000-5000 r--p 00000000 fe:49 4 /data/data/com.hexadecinull.vineos/files/i/rootless/rootfs/vendor/lib64/libvine_sensors_shim.so\n";
	char out[1024];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    out, sizeof out);
	test_int_eq(n > 0, 1);
	out[n] = 0;
	test_str_eq(out,
		"1000-2000 r-xp 00000000 fe:49 1 /system/lib64/libc.so\n"
		"4000-5000 r--p 00000000 fe:49 4 /vendor/lib64/libvine_sensors_shim.so\n");
}

test(maps_output_too_small_returns_nospc)
{
	tawcroot_proc_rewrite_ctx ctx = {
		.rootfs_host_path     = "/data/rootfs",
		.rootfs_host_path_len = 12,
		.binds                = nullptr,
		.n_binds              = 0,
	};
	const char in[] =
		"7f1234567000-7f1234589000 r-xp 00000000 08:01 1234567 /data/rootfs/usr/lib/libfoo.so\n";
	char small[16];
	long n = tawcroot_proc_maps_rewrite(&ctx, in, strlen(in),
					    small, sizeof small);
	test_int_eq(n, -28);  /* -ENOSPC */
}

/* --- /proc/cpuinfo: hide SVE cores ------------------------------- */

static long hide_sve(const char *in, char *out, size_t cap)
{
	long n = tawcroot_cpuinfo_hide_sve_cores(in, strlen(in), out, cap);
	if (n >= 0 && (size_t)n < cap) out[n] = 0;
	return n;
}

/* Snapdragon 8+ Gen 1 (SM8475): A510 / A710 / X2 with SVE off. */
test(cpuinfo_sve_parts_become_a78)
{
	const char in[] =
		"processor\t: 0\n"
		"Features\t: fp asimd atomics\n"
		"CPU implementer\t: 0x41\n"
		"CPU architecture: 8\n"
		"CPU part\t: 0xd46\n"
		"CPU revision\t: 1\n"
		"\n"
		"processor\t: 7\n"
		"CPU implementer\t: 0x41\n"
		"CPU part\t: 0xd48\n"
		"CPU revision\t: 0\n";
	const char want[] =
		"processor\t: 0\n"
		"Features\t: fp asimd atomics\n"
		"CPU implementer\t: 0x41\n"
		"CPU architecture: 8\n"
		"CPU part\t: 0xd41\n"
		"CPU revision\t: 1\n"
		"\n"
		"processor\t: 7\n"
		"CPU implementer\t: 0x41\n"
		"CPU part\t: 0xd41\n"
		"CPU revision\t: 0\n";
	char out[512];
	test_int_eq(hide_sve(in, out, sizeof out), (long)strlen(want));
	test_str_eq(out, want);
}

/* Cores without SVE keep their part: a Cortex-A55 must not be promoted
 * to an A78, whose v8.2 extras it may lack. */
test(cpuinfo_non_sve_parts_pass_through)
{
	const char in[] =
		"CPU implementer\t: 0x41\n"
		"CPU part\t: 0xd05\n"
		"CPU implementer\t: 0x41\n"
		"CPU part\t: 0xd41\n";
	char out[256];
	test_int_eq(hide_sve(in, out, sizeof out), (long)strlen(in));
	test_str_eq(out, in);
}

/* Part numbers are per implementer: 0xd46 from another vendor is not an
 * Arm Cortex-A510. */
test(cpuinfo_other_implementer_passes_through)
{
	const char in[] =
		"CPU implementer\t: 0x51\n"
		"CPU part\t: 0xd46\n";
	char out[128];
	test_int_eq(hide_sve(in, out, sizeof out), (long)strlen(in));
	test_str_eq(out, in);
}

test(cpuinfo_no_trailing_newline)
{
	const char in[] = "CPU implementer\t: 0x41\nCPU part\t: 0xd47";
	const char want[] = "CPU implementer\t: 0x41\nCPU part\t: 0xd41";
	char out[128];
	test_int_eq(hide_sve(in, out, sizeof out), (long)strlen(want));
	test_str_eq(out, want);
}

test(cpuinfo_output_too_small)
{
	const char in[] = "CPU implementer\t: 0x41\nCPU part\t: 0xd48\n";
	char out[8];
	test_int_eq(tawcroot_cpuinfo_hide_sve_cores(in, strlen(in), out, sizeof out),
		    -28L);  /* -ENOSPC */
}
