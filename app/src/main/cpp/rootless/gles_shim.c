/* The guest's GLES driver library (libGLESv2_angle.so and
 * libGLESv1_CM_angle.so, rootless_prep.cpp) in front of ANGLE: it
 * defines only the queries that describe the GPU. Android's EGL loader
 * takes each GL entry point it can't dlsym here from the EGL driver's
 * eglGetProcAddress, which is ANGLE's (libEGL_angle.so), so everything
 * else runs ANGLE's code directly.
 *
 * Apps see an Adreno 620's strings instead of ANGLE's ("ANGLE (Qualcomm,
 * Vulkan ...)", "Google Inc. (Qualcomm)") and no GL_ANGLE_ / GL_CHROMIUM_
 * extensions; system processes keep ANGLE's, which its users (Skia in
 * SurfaceFlinger) may rely on. The version stays the one ANGLE
 * implements (3.1): apps choose shaders by it. Unverified on a device:
 * the driver build in the version string. */
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <GLES3/gl31.h>

#define EGL_DRIVER "/vendor/lib64/egl/libEGL_angle.so"
#define VENDOR "Qualcomm"
#define RENDERER "Adreno (TM) 620"
#define VERSION_PREFIX "OpenGL ES 3.1 V@0502.0 (GIT@7d8a5d1cd0, I9a9f0b7e8b, 1683677478) (Date:05/09/23)"

typedef const GLubyte* (*get_string_fn)(GLenum);
typedef const GLubyte* (*get_stringi_fn)(GLenum, GLuint);
typedef void (*get_integerv_fn)(GLenum, GLint*);

static get_string_fn real_get_string;
static get_stringi_fn real_get_stringi;
static get_integerv_fn real_get_integerv;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void resolve(void)
{
    void* egl = dlopen(EGL_DRIVER, RTLD_NOW | RTLD_NOLOAD);
    if (!egl) egl = dlopen(EGL_DRIVER, RTLD_NOW);
    void* (*gpa)(const char*) = egl ? (void* (*)(const char*))dlsym(egl, "eglGetProcAddress") : NULL;
    if (!gpa) return;
    real_get_string = (get_string_fn)gpa("glGetString");
    real_get_stringi = (get_stringi_fn)gpa("glGetStringi");
    real_get_integerv = (get_integerv_fn)gpa("glGetIntegerv");
}

static int app(void)
{
    return getuid() % 100000 >= 10000;
}

static int hidden_extension(const char* e)
{
    return !strncmp(e, "GL_ANGLE_", 9) || !strncmp(e, "GL_CHROMIUM_", 12);
}

/* ANGLE's extension string, filtered, per distinct string ANGLE returns
 * (one per context version); kept for the process's life as GL requires */
static const GLubyte* filtered_extensions(const GLubyte* real)
{
    static struct { const GLubyte* real; char* kept; } cache[8];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    if (!real) return real;
    pthread_mutex_lock(&lock);
    const GLubyte* out = real;
    int i = 0;
    for (; i < 8 && cache[i].real; i++)
        if (cache[i].real == real) break;
    if (i < 8 && cache[i].real == real) {
        out = (const GLubyte*)cache[i].kept;
    } else if (i < 8) {
        const char* s = (const char*)real;
        char* kept = malloc(strlen(s) + 1);
        if (kept) {
            size_t n = 0;
            while (*s) {
                const char* e = s;
                while (*e && *e != ' ') e++;
                if (e > s && strncmp(s, "GL_ANGLE_", 9) && strncmp(s, "GL_CHROMIUM_", 12)) {
                    memcpy(kept + n, s, (size_t)(e - s));
                    n += (size_t)(e - s);
                    kept[n++] = ' ';
                }
                s = *e ? e + 1 : e;
            }
            if (n) n--;
            kept[n] = 0;
            cache[i].real = real;
            cache[i].kept = kept;
            out = (const GLubyte*)kept;
        }
    }
    pthread_mutex_unlock(&lock);
    return out;
}

static GLint real_extension_count(void)
{
    GLint n = 0;
    real_get_integerv(GL_NUM_EXTENSIONS, &n);
    return n;
}

/* libEGL treats the driver as ANGLE (ANGLE connects the window surfaces
 * itself) when the GLES library exports these: ANGLE's, forwarded */
#define ANGLE_GLES "/vendor/lib64/egl/libllvm-glnext.so"

static void* angle_symbol(const char* name)
{
    void* gles = dlopen(ANGLE_GLES, RTLD_NOW | RTLD_NOLOAD);
    if (!gles) gles = dlopen(ANGLE_GLES, RTLD_NOW);
    return gles ? dlsym(gles, name) : NULL;
}

typedef int (*platform_fn)(void*, const char* const*, unsigned int, void*, void*);
typedef void (*reset_fn)(void*);

__attribute__((visibility("default"))) int ANGLEGetDisplayPlatform(void* display, const char* const names[],
                                                                   unsigned int count, void* context, void* out)
{
    platform_fn f = (platform_fn)angle_symbol("ANGLEGetDisplayPlatform");
    return f ? f(display, names, count, context, out) : 0;
}

__attribute__((visibility("default"))) void ANGLEResetDisplayPlatform(void* display)
{
    reset_fn f = (reset_fn)angle_symbol("ANGLEResetDisplayPlatform");
    if (f) f(display);
}

__attribute__((visibility("default"))) const GLubyte* glGetString(GLenum name)
{
    pthread_once(&once, resolve);
    if (!real_get_string) return NULL;
    const GLubyte* real = real_get_string(name);
    if (!real || !app()) return real;
    switch (name) {
    case GL_VENDOR: return (const GLubyte*)VENDOR;
    case GL_RENDERER: return (const GLubyte*)RENDERER;
    case GL_VERSION:
        /* GLES 1 contexts: "OpenGL ES-CM 1.1 ..." */
        return strncmp((const char*)real, "OpenGL ES 3", 11) ? real : (const GLubyte*)VERSION_PREFIX;
    case GL_EXTENSIONS: return filtered_extensions(real);
    default: return real;
    }
}

__attribute__((visibility("default"))) const GLubyte* glGetStringi(GLenum name, GLuint index)
{
    pthread_once(&once, resolve);
    if (!real_get_stringi) return NULL;
    if (name != GL_EXTENSIONS || !app() || !real_get_integerv) return real_get_stringi(name, index);
    GLint n = real_extension_count();
    GLuint k = 0;
    for (GLint i = 0; i < n; i++) {
        const GLubyte* e = real_get_stringi(name, (GLuint)i);
        if (!e || hidden_extension((const char*)e)) continue;
        if (k++ == index) return e;
    }
    /* Out of range: ANGLE's own error */
    return real_get_stringi(name, (GLuint)n);
}

__attribute__((visibility("default"))) void glGetIntegerv(GLenum pname, GLint* data)
{
    pthread_once(&once, resolve);
    if (!real_get_integerv) return;
    real_get_integerv(pname, data);
    if (pname != GL_NUM_EXTENSIONS || !app() || !real_get_stringi || !data) return;
    GLint kept = 0;
    for (GLint i = 0; i < *data; i++) {
        const GLubyte* e = real_get_stringi(GL_EXTENSIONS, (GLuint)i);
        if (e && !hidden_extension((const char*)e)) kept++;
    }
    *data = kept;
}
