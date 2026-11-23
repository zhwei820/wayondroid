/* Preloaded into a rootless guest's sensors HAL service (the emulator's
 * android.hardware.sensors@2.1 multihal with the goldfish sub-HAL).
 *
 * The sub-HAL reaches the emulator's host over vsock port 5000, else
 * /dev/goldfish_pipe. Here vsock fails and opening the pipe connects to
 * vine_sensors on the phone, through /run/user/1000/vine-sensors.
 *
 * Without vine_sensors the sub-HAL would abort, init would restart it,
 * and system_server, which waits for the declared HAL, would never get
 * past SensorService: then the pipe's other end is local and reports no
 * sensors. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

static const char kPipe[] = "/dev/goldfish_pipe";
static const char kHost[] = "/run/user/1000/vine-sensors";

typedef int (*socket_fn)(int, int, int);
typedef int (*open_fn)(const char*, int, ...);
typedef int (*open2_fn)(const char*, int);

static int real_socket(int domain, int type, int protocol)
{
    static socket_fn fn;
    if (!fn) fn = (socket_fn)dlsym(RTLD_NEXT, "socket");
    return fn(domain, type, protocol);
}

static int read_full(int fd, void* buf, size_t n)
{
    char* p = buf;
    while (n) {
        const ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* A host with no sensors: "0" to list-sensors, everything else ignored */
static void* no_sensors(void* arg)
{
    const int fd = (int)(long)arg;
    char c;
    do {
        if (read_full(fd, &c, 1)) goto out;
    } while (c);
    for (;;) {
        char hdr[5] = {0}, msg[256];
        unsigned len;
        if (read_full(fd, hdr, 4) || sscanf(hdr, "%04x", &len) != 1 || len >= sizeof msg || read_full(fd, msg, len))
            break;
        msg[len] = 0;
        if (!strcmp(msg, "list-sensors")) write(fd, "00010", 5);
    }
out:
    close(fd);
    return NULL;
}

static int local_host(int flags)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return -1;
    pthread_t t;
    if (pthread_create(&t, NULL, no_sensors, (void*)(long)sv[1]) != 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    pthread_detach(t);
    if (!(flags & O_CLOEXEC)) fcntl(sv[0], F_SETFD, 0);
    return sv[0];
}

static int open_pipe(int flags)
{
    const int fd = real_socket(AF_UNIX, SOCK_STREAM | ((flags & O_CLOEXEC) ? SOCK_CLOEXEC : 0), 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    memcpy(sa.sun_path, kHost, sizeof kHost);
    if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
        close(fd);
        return local_host(flags);
    }
    if (flags & O_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

EXPORT int socket(int domain, int type, int protocol)
{
    if (domain == AF_VSOCK) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return real_socket(domain, type, protocol);
}

EXPORT int open(const char* path, int flags, ...)
{
    if (path && !strcmp(path, kPipe)) return open_pipe(flags);
    int mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    static open_fn fn;
    if (!fn) fn = (open_fn)dlsym(RTLD_NEXT, "open");
    return fn(path, flags, mode);
}

EXPORT int __open_2(const char* path, int flags)
{
    if (path && !strcmp(path, kPipe)) return open_pipe(flags);
    static open2_fn fn;
    if (!fn) fn = (open2_fn)dlsym(RTLD_NEXT, "__open_2");
    return fn(path, flags);
}
