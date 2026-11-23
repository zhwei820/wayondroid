/* Unit tests for tawcroot_proctitle_point_argv (tawcroot/src/proctitle.c):
 * the guest's argv strings must be the kernel arg region itself, so a
 * guest renaming itself through argv[0] shows in /proc/<pid>/cmdline.
 * A char array stands in for the region; exec_path NULL skips the
 * PR_SET_NAME that would rename the test runner. */

#include <cleat/test.h>
#include <stdint.h>
#include <string.h>

#include "proctitle.h"

/* Region like a re-exec's: "tawcroot" then one argument the exec
 * handler sized to fit the guest cmdline, 64 bytes in all. */
static char region[64];

static void stash_region(void)
{
	memset(region, 'p', sizeof region);
	memcpy(region, "tawcroot", 9);
	region[sizeof region - 1] = 0;
	char *argv[] = { region, region + 9 };
	tawcroot_proctitle_stash(2, argv);
}

test(proctitle_argv_points_into_region)
{
	stash_region();
	const char *const gargv[] = { "/system/bin/app_process64", "-Xzygote", "/system/bin", NULL };
	tawcroot_proctitle_apply(NULL, 3, gargv);
	uintptr_t slots[3] = { 0 };
	test_int_eq(tawcroot_proctitle_point_argv(slots, 3), 1);
	test_true(slots[0] == (uintptr_t)region);
	test_str_eq((const char *)slots[0], "/system/bin/app_process64");
	test_str_eq((const char *)slots[1], "-Xzygote");
	test_str_eq((const char *)slots[2], "/system/bin");
}

/* The zygote's rename: overwrite the argv block in place, as
 * AndroidRuntime::setArgv0 does. The "cmdline" is now the package. */
test(proctitle_guest_rename_reaches_region)
{
	stash_region();
	const char *const gargv[] = { "/system/bin/app_process64", "-Xzygote", NULL };
	tawcroot_proctitle_apply(NULL, 2, gargv);
	uintptr_t slots[2] = { 0 };
	test_int_eq(tawcroot_proctitle_point_argv(slots, 2), 1);
	char *block = (char *)slots[0];
	size_t block_len = (size_t)((char *)slots[1] + strlen((char *)slots[1]) + 1 - block);
	memset(block, 0, block_len);
	strcpy(block, "com.apkpure.aegon");
	test_str_eq(region, "com.apkpure.aegon");
}

/* A cmdline longer than the region is truncated; argv keeps its own
 * copies rather than pointing at a cut-off string. */
test(proctitle_truncated_keeps_copies)
{
	stash_region();
	char longarg[200];
	memset(longarg, 'a', sizeof longarg - 1);
	longarg[sizeof longarg - 1] = 0;
	const char *const gargv[] = { "/bin/sh", longarg, NULL };
	tawcroot_proctitle_apply(NULL, 2, gargv);
	uintptr_t slots[2] = { 11, 22 };
	test_int_eq(tawcroot_proctitle_point_argv(slots, 2), 0);
	test_true(slots[0] == 11 && slots[1] == 22);
}
