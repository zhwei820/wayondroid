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
 * sensors.
 *
 * It also turns the sub-HAL's sensor table into a Pixel 5's (as its
 * vendor.img and ADSP firmware name the parts): per-part vendors, a
 * wake-up proximity sensor, and the three hinge-angle slots, which
 * vine_sensors feeds, as the step counter, step detector and significant
 * motion sensors. The sub-HAL posts their events as hinge angles;
 * postSensorEvent, which it calls through its PLT, is interposed to give
 * them their types and payloads. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#ifndef AF_VSOCK
#define AF_VSOCK 40
#endif

static const char kPipe[] = "/dev/goldfish_pipe";
static const char kSubHal[] = "/sensors.ssc.so";
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

/* android::hardware::sensors V1_0::SensorInfo (V2_1's is the same) and
 * hidl_string, LP64 */
struct hstr {
    const char* buf;
    uint32_t size;
    bool owns;
};
struct sensor_info {
    int32_t handle;
    struct hstr name, vendor;
    int32_t version, type;
    struct hstr type_string;
    float max_range, resolution, power;
    int32_t min_delay;
    uint32_t fifo_reserved, fifo_max;
    struct hstr permission;
    int32_t max_delay;
    uint32_t flags;
};
#if defined(__LP64__)
_Static_assert(sizeof(struct sensor_info) == 0x70, "SensorInfo is 0x70 bytes");
#endif
/* V2_1::Event */
struct event {
    int64_t timestamp;
    int32_t handle, type;
    union {
        float data[16];
        uint64_t step_count;
    } u;
};

/* The goldfish handles (sensor_list.h) */
enum { kProximity = 5, kLight = 6, kPressure = 7, kHinge0 = 11, kHinge1 = 12, kHinge2 = 13, kSensors = 18 };
enum { kTypeSignificantMotion = 17, kTypeStepDetector = 18, kTypeStepCounter = 19, kTypeHinge = 36 };
/* Phones report a binary proximity sensor's "far" as its range in cm; the
 * goldfish one's range is 1 */
static const float kProximityFar = 5.0f;
enum { kWakeUp = 1, kOnChange = 2, kOneShot = 4, kSpecial = 6 };

static const struct {
    int handle, type;
    const char *name, *vendor, *type_string, *permission;
    uint32_t flags;
    int32_t min_delay;
    float max_range, power;
} kRetype[] = {
    {kHinge0, kTypeStepCounter, "Step Counter", "Google", "android.sensor.step_counter",
     "android.permission.ACTIVITY_RECOGNITION", kOnChange, 0, 4294967296.0f, 0.001f},
    {kHinge1, kTypeStepDetector, "Step Detector", "Google", "android.sensor.step_detector",
     "android.permission.ACTIVITY_RECOGNITION", kSpecial, 0, 1.0f, 0.001f},
    {kHinge2, kTypeSignificantMotion, "Significant Motion", "Google", "android.sensor.significant_motion", "",
     kOneShot | kWakeUp, -1, 1.0f, 0.001f},
};

static void set_str(struct hstr* h, const char* text)
{
    h->buf = text;
    h->size = (uint32_t)strlen(text);
    h->owns = false; /* the old buffer leaks: the table lives as long as the process */
}

static char subhal_path[256];
static struct sensor_info* table; /* the sub-HAL's, once patched */

static int find_table(struct dl_phdr_info* info, size_t size, void* out)
{
    (void)size;
    const size_t n = info->dlpi_name ? strlen(info->dlpi_name) : 0;
    if (n < sizeof kSubHal - 1 || strcmp(info->dlpi_name + n - (sizeof kSubHal - 1), kSubHal)) return 0;
    snprintf(subhal_path, sizeof subhal_path, "%s", info->dlpi_name);
    for (int p = 0; p < info->dlpi_phnum; p++) {
        const ElfW(Phdr)* ph = &info->dlpi_phdr[p];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) continue;
        const uintptr_t lo = info->dlpi_addr + ph->p_vaddr;
        const uintptr_t hi = lo + ph->p_memsz;
        for (uintptr_t a = (lo + 7) & ~(uintptr_t)7; a + kSensors * sizeof(struct sensor_info) <= hi; a += 8) {
            struct sensor_info* t = (struct sensor_info*)a;
            int k = 1;
            while (k < 11 && t[k].handle == k) k++;
            if (k < 11 || t[kHinge0].handle != kHinge0 || t[kHinge0].type != kTypeHinge ||
                t[kHinge2].handle != kHinge2 || t[kHinge2].type != kTypeHinge || !t[1].type_string.buf ||
                strcmp(t[1].type_string.buf, "android.sensor.gyroscope"))
                continue;
            *(struct sensor_info**)out = t;
            return 1;
        }
    }
    return 1;
}

/* Before the sub-HAL lists its sensors: it opens the pipe to ask which
 * there are */
static void patch_table(void)
{
    static bool done;
    if (done) return;
    done = true;
#if !defined(__LP64__)
    return; /* the guest's HAL is 64-bit; the layout above is LP64's */
#endif
    struct sensor_info* t = NULL;
    dl_iterate_phdr(find_table, &t);
    if (!t) return;
    table = t;
    set_str(&t[kPressure].vendor, "Bosch");
    set_str(&t[kProximity].vendor, "AMS");
    set_str(&t[kLight].vendor, "AMS");
    t[kProximity].flags |= kWakeUp;
    t[kProximity].max_range = kProximityFar;
    for (size_t i = 0; i < sizeof kRetype / sizeof kRetype[0]; i++) {
        struct sensor_info* s = &t[kRetype[i].handle];
        s->type = kRetype[i].type;
        set_str(&s->name, kRetype[i].name);
        set_str(&s->vendor, kRetype[i].vendor);
        set_str(&s->type_string, kRetype[i].type_string);
        set_str(&s->permission, kRetype[i].permission);
        s->flags = kRetype[i].flags;
        s->min_delay = kRetype[i].min_delay;
        s->max_delay = 0;
        s->max_range = kRetype[i].max_range;
        s->resolution = 1.0f;
        s->power = kRetype[i].power;
        s->fifo_reserved = s->fifo_max = 0;
    }
}

typedef void (*post_fn)(void*, const struct event*);
typedef void (*info_fn)(void*, int);
typedef void (*post_locked_fn)(void*, const struct sensor_info*, const struct event*);

#define POST_SENSOR_EVENT _ZN8goldfish15MultihalSensors15postSensorEventERKN7android8hardware7sensors4V2_15EventE
#define SEND_INFO_REPORT _ZN8goldfish15MultihalSensors24sendAdditionalInfoReportEi
#define POST_LOCKED \
    _ZN8goldfish15MultihalSensors23doPostSensorEventLockedERKN7android8hardware7sensors4V2_110SensorInfoERKNS4_5EventE
#define STR2(x) #x
#define STR(x) STR2(x)
EXPORT void POST_SENSOR_EVENT(void* self, const struct event* e);
EXPORT void SEND_INFO_REPORT(void* self, int handle);

/* The sub-HAL is dlopen()ed after this preload, outside its search scope:
 * RTLD_NEXT finds nothing, so look in the sub-HAL itself */
static void* subhal_sym(const char* name, void* self_fn)
{
    void* fn = NULL;
    void* h = subhal_path[0] ? dlopen(subhal_path, RTLD_NOW | RTLD_NOLOAD) : NULL;
    if (h) fn = dlsym(h, name);
    if (!fn) fn = dlsym(RTLD_NEXT, name);
    return fn == self_fn ? NULL : fn;
}

static post_fn real_post(void)
{
    static post_fn real;
    if (!real) real = (post_fn)subhal_sym(STR(POST_SENSOR_EVENT), (void*)POST_SENSOR_EVENT);
    return real;
}

/* On-change sensors report their current value when activated (the CDD's
 * rule; the framework only replays it to clients that join an active
 * sensor). The sub-HAL posts a value only when the host's changes and drops
 * it while the sensor is off, so the last one is kept here and posted from
 * activate(), which calls sendAdditionalInfoReport with the HAL's lock held
 * (flush() does too, after its flush-complete event: a repeat is harmless). */
/* The sub-HAL polls, at the sampling rate, every active sensor that isn't
 * on-change (MultihalSensors::activate), and the framework gives a one-shot
 * sensor no rate: significant motion would spin it. HalProxy takes the
 * sensor list (with its one-shot flags) when it loads the sub-HAL, before
 * initialize() turns the host's sensors on; after that the sub-HAL's own
 * lookups see an on-change sensor. */
#define GET_INFO _ZN8goldfish21getSensorInfoByHandleEi
#define SET_ALL _ZN8goldfish15MultihalSensors17setAllQemuSensorsEb
EXPORT const struct sensor_info* GET_INFO(int handle);
EXPORT bool SET_ALL(void* self, bool enabled);
static volatile bool listed;

EXPORT bool SET_ALL(void* self, bool enabled)
{
    static bool (*real)(void*, bool);
    if (!real) real = (bool (*)(void*, bool))subhal_sym(STR(SET_ALL), (void*)SET_ALL);
    if (enabled) listed = true;
    return real ? real(self, enabled) : false;
}

EXPORT const struct sensor_info* GET_INFO(int handle)
{
    static const struct sensor_info* (*real)(int);
    if (!real) real = (const struct sensor_info* (*)(int))subhal_sym(STR(GET_INFO), (void*)GET_INFO);
    const struct sensor_info* s = real ? real(handle) : NULL;
    if (!s || !listed || handle != kHinge2 || s->type != kTypeSignificantMotion) return s;
    static struct sensor_info on_change;
    on_change = *s;
    on_change.flags = (s->flags & kWakeUp) | kOnChange;
    return &on_change;
}

static pthread_mutex_t last_lock = PTHREAD_MUTEX_INITIALIZER;
static struct event last[kSensors];
static bool have_last[kSensors];

static bool on_change(int handle)
{
    return table && handle >= 0 && handle < kSensors && ((table[handle].flags >> 1) & 7) == 1;
}

static void remember(const struct event* e)
{
    if (!on_change(e->handle)) return;
    pthread_mutex_lock(&last_lock);
    last[e->handle] = *e;
    have_last[e->handle] = true;
    pthread_mutex_unlock(&last_lock);
}

EXPORT void SEND_INFO_REPORT(void* self, int handle)
{
    static info_fn real;
    static post_locked_fn post_locked;
    if (!real) real = (info_fn)subhal_sym(STR(SEND_INFO_REPORT), (void*)SEND_INFO_REPORT);
    if (!post_locked) post_locked = (post_locked_fn)subhal_sym(STR(POST_LOCKED), NULL);
    if (real) real(self, handle);
    if (!post_locked || !on_change(handle)) return;
    struct event e;
    pthread_mutex_lock(&last_lock);
    const bool have = have_last[handle];
    e = last[handle];
    pthread_mutex_unlock(&last_lock);
    if (!have) {
        if (handle != kHinge0) return;
        /* no step since the guest booted */
        memset(&e, 0, sizeof e);
        e.handle = kHinge0;
        e.type = kTypeStepCounter;
    }
    struct timespec now;
    clock_gettime(CLOCK_BOOTTIME, &now); /* elapsedRealtimeNano(), the sub-HAL's clock */
    e.timestamp = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    post_locked(self, &table[handle], &e);
}

/* vine_sensors sends hinge angles: the step count, and for the detector
 * and significant motion a value that changes on each event (the sub-HAL
 * posts a hinge angle only when it changes) */
EXPORT void POST_SENSOR_EVENT(void* self, const struct event* e)
{
    const post_fn real = real_post();
    if (!real) return;
    if (e->handle == kProximity) {
        struct event out = *e;
        out.u.data[0] = e->u.data[0] > 0.5f ? kProximityFar : 0.0f;
        remember(&out);
        real(self, &out);
        return;
    }
    if (e->type != kTypeHinge || e->handle < kHinge0 || e->handle > kHinge2) {
        remember(e);
        real(self, e);
        return;
    }
    struct event out = *e;
    memset(&out.u, 0, sizeof out.u);
    if (e->handle == kHinge0) {
        out.type = kTypeStepCounter;
        out.u.step_count = e->u.data[0] > 0 ? (uint64_t)e->u.data[0] : 0;
    } else {
        out.type = e->handle == kHinge1 ? kTypeStepDetector : kTypeSignificantMotion;
        out.u.data[0] = 1.0f;
    }
    remember(&out);
    real(self, &out);
}

static int open_pipe(int flags)
{
    patch_table();
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
