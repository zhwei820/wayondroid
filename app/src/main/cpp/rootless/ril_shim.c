/* Preloaded into a rootless guest's RIL (the emulator's libgoldfish-rild
 * with libcuttlefish-ril-2.so).
 *
 * Outside the emulator that RIL always dials its modem over vsock: its
 * port variable is unsigned, so the "-1 means none" test never holds and
 * its other transports are unreachable. A vsock socket can't be made here
 * and the RIL retries at once, forever. Here the vsock socket is a unix
 * one and the connect, to any vsock port, reaches vine_modem through
 * /run/user/1000/vine-modem.
 *
 * The RIL's name (gsm.version.ril-impl) and baseband version are fixed
 * strings that say "reference-ril"; RIL_IMPL_NAME and RIL_BASEBAND_VERSION
 * (the rc's setenv) replace them. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
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

/* telephony/ril.h. onRequest takes a fifth argument, the socket id, in
 * multi-SIM builds: passed on whether there or not. */
#define RIL_REQUEST_BASEBAND_VERSION 51
#define RIL_E_SUCCESS 0
typedef void* RIL_Token;
typedef void (*request_fn)(int request, void* data, size_t len, RIL_Token t, int socket_id);
typedef struct {
    int version;
    request_fn onRequest;
    void* onStateRequest;
    void* supports;
    void* onCancel;
    const char* (*getVersion)(void);
} RIL_RadioFunctions;

typedef void (*register_fn)(const RIL_RadioFunctions*);
typedef void (*complete_fn)(RIL_Token, int, void*, size_t);

static RIL_RadioFunctions s_funcs;
static request_fn s_on_request;
static complete_fn s_complete;
static const char *s_impl, *s_baseband;

static const char* get_version(void) { return s_impl; }

static void on_request(int request, void* data, size_t len, RIL_Token t, int socket_id)
{
    if (request == RIL_REQUEST_BASEBAND_VERSION && s_baseband && s_complete) {
        /* A string response is the char* itself */
        s_complete(t, RIL_E_SUCCESS, (void*)s_baseband, sizeof(char*));
        return;
    }
    s_on_request(request, data, len, t, socket_id);
}

EXPORT void RIL_register(const RIL_RadioFunctions* funcs)
{
    register_fn real = (register_fn)dlsym(RTLD_NEXT, "RIL_register");
    s_complete = (complete_fn)dlsym(RTLD_NEXT, "RIL_onRequestComplete");
    s_impl = getenv("RIL_IMPL_NAME");
    s_baseband = getenv("RIL_BASEBAND_VERSION");
    if (!funcs) {
        real(funcs);
        return;
    }
    s_funcs = *funcs;
    s_on_request = funcs->onRequest;
    s_funcs.onRequest = on_request;
    if (s_impl && funcs->getVersion) s_funcs.getVersion = get_version;
    real(&s_funcs);
}
