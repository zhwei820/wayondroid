/* Preloaded into a rootless guest's composer service (composer@2.1 over
 * HWC2On1Adapter): gives Waydroid's hwcomposer, which has one display
 * config, a Pixel 5's two display modes, 60 and 90 Hz, in that order (the
 * framework numbers modes as the HAL lists them). The hwcomposer's own
 * rate stays the identity's 90 Hz; while SurfaceFlinger has picked 60 Hz,
 * its vsync events go out on a 60 Hz grid, each when the first 90 Hz vsync
 * at or after its time arrives.
 *
 * Touches: the hwcomposer's evdev batches for a touch-down or touch-move
 * (ABS_MT_PRESSURE 50, no contact size) get the phone's pressure and
 * contact axes from vine_wl's ring (touch_extras.h), one record per batch
 * in order. Only write(2) is wrapped, and only such batches are changed.
 *
 * Layouts from hardware/libhardware hardware.h and hwcomposer.h
 * (android13-release), LP64. */
#define _GNU_SOURCE
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include "../display/wayland/touch_extras.h"
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))

struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
    int (*open)(const struct hw_module_t* module, const char* id, struct hw_device_t** device);
};
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version, hal_api_version;
    const char *id, *name, *author;
    struct hw_module_methods_t* methods;
    void* dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag, version;
    struct hw_module_t* module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t* device);
};
struct hwc_procs {
    void (*invalidate)(const struct hwc_procs* procs);
    void (*vsync)(const struct hwc_procs* procs, int disp, int64_t timestamp);
    void (*hotplug)(const struct hwc_procs* procs, int disp, int connected);
};
struct hwc_dev {
    struct hw_device_t common;
    void *prepare, *set, *event_control, *set_power_mode, *query;
    void (*register_procs)(struct hwc_dev* dev, const struct hwc_procs* procs);
    void* dump;
    int (*get_display_configs)(struct hwc_dev* dev, int disp, uint32_t* configs, size_t* num);
    int (*get_display_attributes)(struct hwc_dev* dev, int disp, uint32_t config, const uint32_t* attributes,
                                  int32_t* values);
    int (*get_active_config)(struct hwc_dev* dev, int disp);
    int (*set_active_config)(struct hwc_dev* dev, int disp, int index);
    void* set_cursor_position_async;
    void* reserved_proc[1];
};
#if defined(__LP64__)
_Static_assert(sizeof(struct hw_device_t) == 120, "hw_device_t is 120 bytes");
_Static_assert(sizeof(struct hw_module_t) == 248, "hw_module_t is 248 bytes");
#endif

enum { kPrimary = 0, kNoAttribute = 0, kVsyncPeriod = 1 };
enum { kConfig60, kConfigNative, kConfigs };
static const int64_t kPeriod60 = 1000000000 / 60;

static const struct hw_module_t* real_module;
static struct hw_module_t module_copy;
static struct hw_module_methods_t methods;

static struct {
    int (*get_display_attributes)(struct hwc_dev*, int, uint32_t, const uint32_t*, int32_t*);
    void (*register_procs)(struct hwc_dev*, const struct hwc_procs*);
} real;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int active = kConfigNative;
static int64_t next_vsync; /* the 60 Hz grid's next time, 0 to restart it */
static const struct hwc_procs* sf_procs;

static int get_display_configs(struct hwc_dev* dev, int disp, uint32_t* configs, size_t* num)
{
    (void)dev;
    if (disp != kPrimary) return -EINVAL;
    if (*num == 0) return 0;
    size_t n = *num < kConfigs ? *num : kConfigs;
    for (size_t i = 0; i < n; i++) configs[i] = (uint32_t)i;
    *num = n;
    return 0;
}

static int get_display_attributes(struct hwc_dev* dev, int disp, uint32_t config, const uint32_t* attributes,
                                  int32_t* values)
{
    if (config >= kConfigs) return -EINVAL;
    int r = real.get_display_attributes(dev, disp, 0, attributes, values);
    if (r != 0 || config != kConfig60) return r;
    for (int i = 0; attributes[i] != kNoAttribute; i++)
        if (attributes[i] == kVsyncPeriod) values[i] = (int32_t)kPeriod60;
    return 0;
}

static int get_active_config(struct hwc_dev* dev, int disp)
{
    (void)dev;
    if (disp != kPrimary) return -1;
    pthread_mutex_lock(&lock);
    int a = active;
    pthread_mutex_unlock(&lock);
    return a;
}

static int set_active_config(struct hwc_dev* dev, int disp, int index)
{
    (void)dev;
    if (disp != kPrimary || index < 0 || index >= kConfigs) return -EINVAL;
    pthread_mutex_lock(&lock);
    if (active != index) next_vsync = 0;
    active = index;
    pthread_mutex_unlock(&lock);
    return 0;
}

static void on_invalidate(const struct hwc_procs* procs)
{
    (void)procs;
    if (sf_procs && sf_procs->invalidate) sf_procs->invalidate(sf_procs);
}

static void on_hotplug(const struct hwc_procs* procs, int disp, int connected)
{
    (void)procs;
    if (sf_procs && sf_procs->hotplug) sf_procs->hotplug(sf_procs, disp, connected);
}

static void on_vsync(const struct hwc_procs* procs, int disp, int64_t timestamp)
{
    (void)procs;
    if (!sf_procs || !sf_procs->vsync) return;
    int64_t out = timestamp;
    pthread_mutex_lock(&lock);
    if (disp == kPrimary && active == kConfig60) {
        if (next_vsync == 0 || timestamp - next_vsync > 2 * kPeriod60) next_vsync = timestamp;
        if (timestamp < next_vsync) out = -1;
        else {
            out = next_vsync;
            next_vsync += kPeriod60;
        }
    }
    pthread_mutex_unlock(&lock);
    if (out >= 0) sf_procs->vsync(sf_procs, disp, out);
}

static const struct hwc_procs procs = {on_invalidate, on_vsync, on_hotplug};

static void register_procs(struct hwc_dev* dev, const struct hwc_procs* p)
{
    sf_procs = p;
    real.register_procs(dev, p ? &procs : NULL);
}

static int open_device(const struct hw_module_t* module, const char* id, struct hw_device_t** device)
{
    (void)module;
    int r = real_module->methods->open(real_module, id, device);
    if (r != 0 || !*device) return r;
    struct hwc_dev* dev = (struct hwc_dev*)*device;
    /* configs need HWC 1.4 */
    if ((dev->common.version >> 16) < 0x0104 || !dev->get_display_attributes || !dev->register_procs) return r;
    real.get_display_attributes = dev->get_display_attributes;
    real.register_procs = dev->register_procs;
    dev->get_display_configs = get_display_configs;
    dev->get_display_attributes = get_display_attributes;
    dev->get_active_config = get_active_config;
    dev->set_active_config = set_active_config;
    dev->register_procs = register_procs;
    return r;
}

static int wrap(const char* id, const struct hw_module_t** module, int r)
{
    if (r != 0 || !id || strcmp(id, "hwcomposer") || !*module || !(*module)->methods) return r;
    if (!real_module) {
        real_module = *module;
        module_copy = **module;
        methods.open = open_device;
        module_copy.methods = &methods;
    }
    if (*module == real_module) *module = &module_copy;
    return r;
}

EXPORT int hw_get_module(const char* id, const struct hw_module_t** module)
{
    static int (*fn)(const char*, const struct hw_module_t**);
    if (!fn) fn = (int (*)(const char*, const struct hw_module_t**))dlsym(RTLD_NEXT, "hw_get_module");
    if (!fn) return -ENOENT;
    return wrap(id, module, fn(id, module));
}

EXPORT int hw_get_module_by_class(const char* class_id, const char* inst, const struct hw_module_t** module)
{
    static int (*fn)(const char*, const char*, const struct hw_module_t**);
    if (!fn) fn = (int (*)(const char*, const char*, const struct hw_module_t**))dlsym(RTLD_NEXT, "hw_get_module_by_class");
    if (!fn) return -ENOENT;
    return wrap(inst ? NULL : class_id, module, fn(class_id, inst, module));
}

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
