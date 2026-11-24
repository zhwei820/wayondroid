/* Preloaded into a rootless guest's RIL (the emulator's libgoldfish-rild
 * with libcuttlefish-ril-2.so).
 *
 * Outside the emulator that RIL always dials its modem over vsock: its
 * port variable is unsigned, so the "-1 means none" test never holds and
 * its other transports are unreachable. A vsock socket can't be made here
 * and the RIL retries at once, forever. Here the vsock socket is a unix
 * one and the connect, to any vsock port, reaches vine_modem through
 * /run/user/1000/vine-modem. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

static const char kModem[] = "/run/user/1000/vine-modem";

typedef int (*socket_fn)(int, int, int);
typedef int (*connect_fn)(int, const struct sockaddr*, socklen_t);

EXPORT int socket(int domain, int type, int protocol)
{
    static socket_fn fn;
    if (!fn) fn = (socket_fn)dlsym(RTLD_NEXT, "socket");
    if (domain == AF_VSOCK) return fn(AF_UNIX, type, 0);
    return fn(domain, type, protocol);
}

EXPORT int connect(int fd, const struct sockaddr* addr, socklen_t len)
{
    static connect_fn fn;
    if (!fn) fn = (connect_fn)dlsym(RTLD_NEXT, "connect");
    if (addr && len >= sizeof(sa_family_t) && addr->sa_family == AF_VSOCK) {
        struct sockaddr_un sa = {.sun_family = AF_UNIX};
        memcpy(sa.sun_path, kModem, sizeof kModem);
        const int r = fn(fd, (const struct sockaddr*)&sa, sizeof sa);
        /* The RIL closes and retries at once: slow it to the pace of its
         * other transports' retries while vine_modem is not there */
        if (r != 0) {
            const int e = errno;
            sleep(1);
            errno = e;
        }
        return r;
    }
    return fn(fd, addr, len);
}
