/* The device a guest poses as, from files the host writes under
 * <store>/device before init starts (the rootless engine writes a Pixel
 * 5's). Each view keeps the host's when its file is absent.
 *
 *   cpuinfo        /proc/cpuinfo, for apps
 *   sys-cpu/<p>    /sys/devices/system/cpu/<p>, for apps; a file at a time,
 *                  other files there stay the kernel's
 *   uname          "release\nversion\n": uname(2) for apps and for
 *                  app_process64 (zygote, which sets apps' os.version)
 *   meminfo-kb     a smaller MemTotal: /proc/meminfo's kB values and
 *                  sysinfo(2)'s RAM scaled to it, for every process
 *   data-bytes     a smaller size for the f2fs/ext4 the guest lives on:
 *                  statfs(2) blocks and inodes scaled to it, for every process
 *
 *   hwcap          masks for the AT_HWCAP/AT_HWCAP2 every exec'd guest gets
 *   unlisted       paths apps' directory listings leave out (they still open)
 *   unlistable     directories apps may not open for reading (EACCES)
 *   mounts         apps' /proc/<pid>/mounts (mountinfo made from it), and
 *                  their own profile mounts after it
 *
 * /proc/self/auxv is the auxv the loader built, for every guest process.
 * With the directory present, apps' /proc/self/status also reads as a
 * phone app's: one seccomp filter (tawcroot stacks several), all CPUs. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>
#include "dispatch.h"

/* With a 4.x kernel in the view's uname, the syscalls 4.19 lacks that the
 * host's kernel has (pidfd_send_signal, pidfd_open, close_range,
 * pidfd_getfd, faccessat2, process_madvise) fail with ENOSYS for every
 * guest process, as on the device: callers probe them and fall back.
 * Others newer than 4.19 tawcroot denies anyway (io_uring, clone3,
 * openat2, the new mount API) or the host lacks. */
void tawcroot_device_register(void);

/* A memfd for a guest open of `path`, or TAWC_ENOSYS when the view
 * doesn't replace it */
long tawcroot_device_open(const char *path, int flags);

/* An app's view of a /proc/<pid>/status text of `len` bytes in a buffer
 * of `cap`, rewritten in place; the new length */
size_t tawcroot_device_status(char *buf, size_t len, size_t cap);

/* ANDs the host's AT_HWCAP/AT_HWCAP2 with <store>/device/hwcap ("0x.. 0x..",
 * the device's CPU features), for the auxv the loader builds */
void tawcroot_device_hwcap(uint64_t *hwcap, uint64_t *hwcap2);

/* An app's listing of `dir`: without the unlisted entries, and in the
 * device's cpufreq dir the phone's policy<n> names renamed to the
 * device's, by rank; returns the length kept */
long tawcroot_device_dirents(const char *dir, char *buf, long n);

/* Scales a kernel struct statfs (long[]: type, bsize, blocks, bfree,
 * bavail, files, ffree, ...) in place */
void tawcroot_device_statfs(long *st);

/* Rewrites release and version of a kernel struct new_utsname (six
 * 65-byte fields) at `uts`, guest memory; 0 or -errno */
long tawcroot_device_uname(char *uts);

/* Writes an app's mounts (info: mountinfo) to `out`; TAWC_ENOSYS
 * without a template, for the namespace's own table */
long tawcroot_device_mounts(int out, int info);

/* sysinfo(2), lazily trapped in init sessions */
long tawcroot_device_sysinfo(const tawcroot_syscall_args *a, ucontext_t *uc);
