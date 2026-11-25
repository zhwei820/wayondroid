/* The guest's EGL driver library (libEGL_angle.so, rootless_prep.cpp) in
 * front of ANGLE's, which moves to eglSubDriverAndroid.so beside it. It
 * defines only config selection: Android's EGL loader takes every entry
 * point it can't dlsym here from this library's eglGetProcAddress, which
 * returns ANGLE's.
 *
 * ANGLE on the guest's Vulkan (gfxstream) shows a multisampled window
 * surface as black: the resolve into the swapchain image comes out empty.
 * So apps aren't offered multisampled window configs; ones that ask for
 * one fall back to a single-sampled config (Flutter's Impeller does) or
 * multisample offscreen, which works. System processes keep every config. */
#include <EGL/egl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ANGLE_EGL "/vendor/lib64/egl/eglSubDriverAndroid.so"

typedef __eglMustCastToProperFunctionPointerType (*get_proc_fn)(const char*);
typedef EGLBoolean (*choose_config_fn)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
typedef EGLBoolean (*get_configs_fn)(EGLDisplay, EGLConfig*, EGLint, EGLint*);
typedef EGLBoolean (*get_config_attrib_fn)(EGLDisplay, EGLConfig, EGLint, EGLint*);

static get_proc_fn real_get_proc;
static choose_config_fn real_choose_config;
static get_configs_fn real_get_configs;
static get_config_attrib_fn real_get_config_attrib;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void resolve(void)
{
    void* angle = dlopen(ANGLE_EGL, RTLD_NOW | RTLD_NOLOAD);
    if (!angle) angle = dlopen(ANGLE_EGL, RTLD_NOW);
    if (!angle) return;
    real_get_proc = (get_proc_fn)dlsym(angle, "eglGetProcAddress");
    real_choose_config = (choose_config_fn)dlsym(angle, "eglChooseConfig");
    real_get_configs = (get_configs_fn)dlsym(angle, "eglGetConfigs");
    real_get_config_attrib = (get_config_attrib_fn)dlsym(angle, "eglGetConfigAttrib");
}

static int app(void)
{
    return getuid() % 100000 >= 10000;
}

static int hidden_config(EGLDisplay dpy, EGLConfig c)
{
    EGLint samples = 0, types = 0;
    real_get_config_attrib(dpy, c, EGL_SAMPLES, &samples);
    real_get_config_attrib(dpy, c, EGL_SURFACE_TYPE, &types);
    return samples > 1 && (types & EGL_WINDOW_BIT);
}

/* Keep the visible configs of `all` (in order), into `out` when given:
 * the count EGL reports */
static EGLint keep_visible(EGLDisplay dpy, const EGLConfig* all, EGLint n, EGLConfig* out, EGLint size)
{
    EGLint kept = 0;
    for (EGLint i = 0; i < n; i++) {
        if (hidden_config(dpy, all[i])) continue;
        if (out) {
            if (kept >= size) break;
            out[kept] = all[i];
        }
        kept++;
    }
    return kept;
}

__attribute__((visibility("default"))) EGLBoolean eglChooseConfig(EGLDisplay dpy, const EGLint* attrib_list,
                                                                  EGLConfig* configs, EGLint config_size,
                                                                  EGLint* num_config)
{
    pthread_once(&once, resolve);
    if (!real_choose_config) return EGL_FALSE;
    if (!app() || !real_get_config_attrib || !num_config)
        return real_choose_config(dpy, attrib_list, configs, config_size, num_config);
    EGLint n = 0;
    if (!real_choose_config(dpy, attrib_list, NULL, 0, &n)) return EGL_FALSE;
    EGLConfig* all = n > 0 ? malloc(sizeof(EGLConfig) * (size_t)n) : NULL;
    if (n > 0 && !all) return real_choose_config(dpy, attrib_list, configs, config_size, num_config);
    if (n > 0 && !real_choose_config(dpy, attrib_list, all, n, &n)) {
        free(all);
        return EGL_FALSE;
    }
    *num_config = keep_visible(dpy, all, n, configs, config_size);
    free(all);
    return EGL_TRUE;
}

__attribute__((visibility("default"))) EGLBoolean eglGetConfigs(EGLDisplay dpy, EGLConfig* configs,
                                                               EGLint config_size, EGLint* num_config)
{
    pthread_once(&once, resolve);
    if (!real_get_configs) return EGL_FALSE;
    if (!app() || !real_get_config_attrib || !num_config)
        return real_get_configs(dpy, configs, config_size, num_config);
    EGLint n = 0;
    if (!real_get_configs(dpy, NULL, 0, &n)) return EGL_FALSE;
    EGLConfig* all = n > 0 ? malloc(sizeof(EGLConfig) * (size_t)n) : NULL;
    if (n > 0 && !all) return real_get_configs(dpy, configs, config_size, num_config);
    if (n > 0 && !real_get_configs(dpy, all, n, &n)) {
        free(all);
        return EGL_FALSE;
    }
    *num_config = keep_visible(dpy, all, n, configs, config_size);
    free(all);
    return EGL_TRUE;
}

__attribute__((visibility("default"))) __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char* name)
{
    pthread_once(&once, resolve);
    if (!name || !real_get_proc) return NULL;
    if (!strcmp(name, "eglChooseConfig")) return (__eglMustCastToProperFunctionPointerType)eglChooseConfig;
    if (!strcmp(name, "eglGetConfigs")) return (__eglMustCastToProperFunctionPointerType)eglGetConfigs;
    return real_get_proc(name);
}
