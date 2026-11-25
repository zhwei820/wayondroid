/* Preloaded into a rootless guest's composer service (composer@2.1 over
 * HWC2On1Adapter). Touches: the hwcomposer's evdev batches for a
 * touch-down or touch-move (ABS_MT_PRESSURE 50, no contact size) get the
 * phone's pressure and contact axes from vine_wl's ring (touch_extras.h),
 * one record per batch in order. Only write(2) is wrapped, and only such
 * batches are changed. */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "../display/wayland/touch_extras.h"

#define EXPORT __attribute__((visibility("default")))

/* ---- touches ---- */

struct input_event64 {
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};
_Static_assert(sizeof(struct input_event64) == 24, "LP64 input_event");
enum { kEvSyn = 0, kEvAbs = 3, kAbsMtTouchMajor = 0x30, kAbsMtTouchMinor = 0x31, kAbsMtPositionX = 0x35,
       kAbsMtPressure = 0x3a, kHwcPressure = 50 };

static const struct touch_extras* extras;
static uint32_t extras_read;

static const struct touch_extras* map_extras(void)
{
    static int tried;
    if (extras || tried) return extras;
    tried = 1;
    const char* dir = getenv("XDG_RUNTIME_DIR");
    char path[256];
    if (!dir || (size_t)snprintf(path, sizeof path, "%s/" TOUCH_EXTRAS_FILE, dir) >= sizeof path) return NULL;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    void* p = mmap(NULL, sizeof(struct touch_extras), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    extras = p;
    extras_read = __atomic_load_n(&extras->written, __ATOMIC_ACQUIRE);
    __android_log_print(ANDROID_LOG_INFO, "hwc_shim", "touch extras from %s", path);
    return extras;
}

/* A touch-down or touch-move batch: positions and the fixed pressure */
static int touch_batch(const struct input_event64* e, size_t n)
{
    int position = 0, pressure = 0;
    for (size_t i = 0; i < n; i++) {
        if (e[i].type != kEvAbs) continue;
        if (e[i].code == kAbsMtPositionX) position = 1;
        if (e[i].code == kAbsMtPressure && e[i].value == kHwcPressure) pressure = 1;
    }
    return position && pressure;
}

EXPORT ssize_t write(int fd, const void* buf, size_t count)
{
    static ssize_t (*fn)(int, const void*, size_t);
    if (!fn) fn = (ssize_t (*)(int, const void*, size_t))dlsym(RTLD_NEXT, "write");
    if (!fn) {
        errno = ENOSYS;
        return -1;
    }
    const size_t n = count / sizeof(struct input_event64);
    if (!buf || !count || count % sizeof(struct input_event64) || n > 12 ||
        !touch_batch((const struct input_event64*)buf, n) || !map_extras() ||
        __atomic_load_n(&extras->magic, __ATOMIC_ACQUIRE) != TOUCH_EXTRAS_MAGIC)
        return fn(fd, buf, count);
    const uint32_t written = __atomic_load_n(&extras->written, __ATOMIC_ACQUIRE);
    /* behind by more than the ring holds (or vine_wl restarted): from the newest */
    if (written - extras_read > TOUCH_EXTRAS_SLOTS) extras_read = written - 1;
    if (extras_read == written) return fn(fd, buf, count);
    const struct touch_extra r = extras->slot[extras_read % TOUCH_EXTRAS_SLOTS];
    extras_read++;
    const struct input_event64* in = buf;
    struct input_event64 out[16];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        struct input_event64 e = in[i];
        if (e.type == kEvAbs && e.code == kAbsMtPressure) e.value = r.pressure;
        if (e.type == kEvSyn && e.code == 0) {
            out[k] = e;
            out[k].type = kEvAbs;
            out[k].code = kAbsMtTouchMajor;
            out[k++].value = r.major;
            out[k] = e;
            out[k].type = kEvAbs;
            out[k].code = kAbsMtTouchMinor;
            out[k++].value = r.minor;
        }
        out[k++] = e;
    }
    const ssize_t w = fn(fd, out, k * sizeof *out);
    return w == (ssize_t)(k * sizeof *out) ? (ssize_t)count : w;
}
