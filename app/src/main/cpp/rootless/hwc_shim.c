/* Preloaded into a rootless guest's composer service (composer@2.1 over
 * HWC2On1Adapter): gives Waydroid's hwcomposer, which has one display
 * config, a Pixel 5's two display modes, 60 and 90 Hz, in that order (the
 * framework numbers modes as the HAL lists them). The hwcomposer's own
 * rate stays the identity's 90 Hz; while SurfaceFlinger has picked 60 Hz,
 * its vsync events go out on a 60 Hz grid, each when the first 90 Hz vsync
 * at or after its time arrives.
 *
 * Layouts from hardware/libhardware hardware.h and hwcomposer.h
 * (android13-release), LP64. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
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
