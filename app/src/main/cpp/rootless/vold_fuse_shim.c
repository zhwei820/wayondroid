/* Preloaded into a rootless guest's vold: emulated storage without FUSE.
 *
 * Android 13 mounts /storage/emulated only through FUSE, which an app
 * cannot mount; vold then leaves the volume unmountable and apps see no
 * shared storage. vine-rootless.rc already bind-mounts the lower
 * filesystem (/data/media) where FUSE would be, before zygote starts.
 * This makes vold's part succeed on top of that:
 *   - open("/dev/fuse") gives one end of a socket pair whose other end
 *     holds a FUSE_INIT, all MediaProvider's FUSE daemon needs to report
 *     itself started; it then waits for requests that never come.
 *   - mount(..., "fuse", ...) succeeds without doing anything, and so
 *     do vold's binds of the lower filesystem (pass-through, Android/data
 *     and obb): the rc's binds stand in for them, and more binds of the
 *     same directory would change which path a file's real path shows.
 *   - vold's unmounts under /mnt/user and /mnt/pass_through succeed
 *     without doing anything either: system_server restarting resets
 *     vold, which unmounts every volume and mounts it again, and a real
 *     unmount would take the rc's binds away for good.
 * Apps reach the files through the binds instead of the daemon.
 *
 * Quota project ids, project inheritance and default ACLs on app dirs
 * succeed when the host refuses them (an app may not set them): without
 * sdcardfs vold sets all three in setupAppDir, and one failure fails the
 * whole call, so Android/data/<pkg>/cache, which only vold creates, never
 * appears. Nothing enforces external storage quotas here anyway. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/fuse.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

static int fake_fuse(int flags)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return -1;
    struct {
        struct fuse_in_header h;
        struct fuse_init_in init; /* the pre-7.36 layout */
    } msg;
    memset(&msg, 0, sizeof msg);
    msg.h.len = (uint32_t)(sizeof msg.h + 16);
    msg.h.opcode = FUSE_INIT;
    msg.h.unique = 1;
    msg.init.major = 7;
    msg.init.minor = 31;
    msg.init.max_readahead = 128 * 1024;
    if (send(sv[1], &msg, msg.h.len, 0) != (ssize_t)msg.h.len) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    /* sv[1] stays open (and unread) for the volume's life: a closed peer
     * would read as an unmount to the daemon */
    if (!(flags & O_CLOEXEC)) fcntl(sv[0], F_SETFD, 0);
    return sv[0];
}

static int is_fuse(const char *path) { return path && !strcmp(path, "/dev/fuse"); }

EXPORT int open(const char *path, int flags, ...)
{
    static int (*real)(const char *, int, ...);
    if (is_fuse(path)) return fake_fuse(flags);
    if (!real) real = (int (*)(const char *, int, ...))dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return real(path, flags, mode);
}

/* What a fortified open(path, flags) calls */
EXPORT int __open_2(const char *path, int flags)
{
    static int (*real)(const char *, int);
    if (is_fuse(path)) return fake_fuse(flags);
    if (!real) real = (int (*)(const char *, int))dlsym(RTLD_NEXT, "__open_2");
    return real(path, flags);
}

/* vold builds some paths as "/" + "/data/media/..." */
static const char *one_slash(const char *path)
{
    while (path[0] == '/' && path[1] == '/') path++;
    return path;
}

static int is_storage_target(const char *path)
{
    if (!path) return 0;
    path = one_slash(path);
    return !strncmp(path, "/mnt/user/", 10) || !strncmp(path, "/mnt/pass_through/", 18);
}

EXPORT int mount(const char *source, const char *target, const char *type, unsigned long flags, const void *data)
{
    static int (*real)(const char *, const char *, const char *, unsigned long, const void *);
    if (type && !strcmp(type, "fuse")) return 0;
    if ((flags & MS_BIND) && source &&
        (!strncmp(one_slash(source), "/data/media", 11) || !strncmp(one_slash(source), "/mnt/pass_through/", 18)))
        return 0;
    if (!real) real = (int (*)(const char *, const char *, const char *, unsigned long, const void *))dlsym(RTLD_NEXT, "mount");
    return real(source, target, type, flags, data);
}

EXPORT int umount2(const char *target, int flags)
{
    static int (*real)(const char *, int);
    if (is_storage_target(target)) return 0;
    if (!real) real = (int (*)(const char *, int))dlsym(RTLD_NEXT, "umount2");
    return real(target, flags);
}

EXPORT int umount(const char *target)
{
    static int (*real)(const char *);
    if (is_storage_target(target)) return 0;
    if (!real) real = (int (*)(const char *))dlsym(RTLD_NEXT, "umount");
    return real(target);
}

static int refused(int e) { return e == EACCES || e == EPERM || e == ENOTTY || e == EOPNOTSUPP; }

EXPORT int ioctl(int fd, int request, ...)
{
    static int (*real)(int, int, ...);
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (!real) real = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
    int r = real(fd, request, arg);
    if (r == 0 || !arg || !refused(errno)) return r;
    switch ((unsigned)request) {
    case (unsigned)FS_IOC_FSGETXATTR:
        memset(arg, 0, sizeof(struct fsxattr));
        return 0;
    case (unsigned)FS_IOC_GETFLAGS:
        /* the kernel reads and writes an int, whatever the macro says */
        *(int *)arg = 0;
        return 0;
    case (unsigned)FS_IOC_FSSETXATTR:
    case (unsigned)FS_IOC_SETFLAGS:
        return 0;
    }
    return r;
}

static int is_default_acl(const char *name) { return name && !strcmp(name, "system.posix_acl_default"); }

EXPORT int setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    static int (*real)(const char *, const char *, const void *, size_t, int);
    if (!real) real = (int (*)(const char *, const char *, const void *, size_t, int))dlsym(RTLD_NEXT, "setxattr");
    int r = real(path, name, value, size, flags);
    return r != 0 && is_default_acl(name) && refused(errno) ? 0 : r;
}
