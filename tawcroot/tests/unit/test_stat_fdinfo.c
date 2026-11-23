/* Unit tests for tawcroot_stat_from_fdinfo (tawcroot/src/syscalls_fs.c):
 * the stat synthesized when the LSM denies fstat on an open fd. */

#include <cleat/test.h>
#include <string.h>
#include <sys/stat.h>

#include "syscalls_fs.h"

/* /proc/<zygote>/fdinfo/4 on an SM8475 phone: trace_marker opened
 * O_WRONLY | O_LARGEFILE | O_CLOEXEC. */
test(stat_fdinfo_write_only_tracefs_file)
{
	const char fdinfo[] =
		"pos:\t0\n"
		"flags:\t02400001\n"
		"mnt_id:\t27280\n"
		"ino:\t8733\n";
	struct stat st;
	test_int_eq(tawcroot_stat_from_fdinfo(fdinfo, &st), 0);
	test_true(S_ISREG(st.st_mode));
	test_int_eq((long)(st.st_mode & 07777), 0222L);
	test_int_eq((long)st.st_ino, 8733L);
	test_int_eq((long)st.st_dev, 27280L);
	test_int_eq((long)st.st_nlink, 1L);
}

test(stat_fdinfo_read_write_mode)
{
	const char fdinfo[] = "pos:\t0\nflags:\t02\nmnt_id:\t6\nino:\t42\n";
	struct stat st;
	test_int_eq(tawcroot_stat_from_fdinfo(fdinfo, &st), 0);
	test_int_eq((long)(st.st_mode & 07777), 0666L);
}

/* Same inode on every call: the zygote compares st_dev/st_ino between
 * its fd table snapshot and the re-stat before each fork. */
test(stat_fdinfo_stable_identity)
{
	const char fdinfo[] = "pos:\t0\nflags:\t0100000\nmnt_id:\t9\nino:\t77\n";
	struct stat a, b;
	test_int_eq(tawcroot_stat_from_fdinfo(fdinfo, &a), 0);
	test_int_eq(tawcroot_stat_from_fdinfo(fdinfo, &b), 0);
	test_int_eq((long)a.st_ino, (long)b.st_ino);
	test_int_eq((long)a.st_dev, (long)b.st_dev);
	test_int_eq((long)(a.st_mode & 07777), 0444L);
}

test(stat_fdinfo_missing_ino_is_eacces)
{
	const char fdinfo[] = "pos:\t0\nflags:\t02\nmnt_id:\t6\n";
	struct stat st;
	test_int_eq(tawcroot_stat_from_fdinfo(fdinfo, &st), -13L);
}

/* Keys match at line start only. */
test(stat_fdinfo_key_anchored_at_line_start)
{
	const char fdinfo[] = "pos:\t0\nflags:\t02\nxino:\t5\nino:\t6\n";
	struct stat st;
	test_int_eq(tawcroot_stat_from_fdinfo(fdinfo, &st), 0);
	test_int_eq((long)st.st_ino, 6L);
}
