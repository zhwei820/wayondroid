/* Preloaded into a rootless guest's camera HAL service (Google's camera HAL
 * with the emulator's sensor HWL, rootless_prep.cpp install_camera_hal).
 *
 * The HWL's EmulatedSensor renders a test scene into each output buffer
 * (CaptureYUV420, CaptureRGB, CaptureRawBinned, called through its PLT).
 * Here they are filled from the phone's camera facing the same way,
 * through vine_camera (/run/user/1000/vine-camera, camera/vine_camera.c):
 * the phone's frame cropped to the output's aspect and the request's zoom,
 * turned from the phone camera's sensor orientation to the guest's. JPEG
 * is encoded from YUV, so photos are the phone's view too. Without
 * vine_camera, or before its first frame, the test scene stays.
 *
 * The connection lasts while the guest streams: closed after a second
 * without a capture, which stops the phone's camera. Captures before the
 * phone's first frame get the test scene. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../camera/camera_shm.h"

#define EXPORT __attribute__((visibility("default")))

static const char kHost[] = "/run/user/1000/vine-camera";

/* The guest's sensors (scripts/sensors/fetch-hal.py PIXEL5_CAMERAS): the
 * back one is the larger. SensorCharacteristics starts with its size_t
 * width and height. */
#define BACK_WIDTH 4032
#define FRONT_WIDTH 3264
#define BACK_ORIENTATION 90
#define FRONT_ORIENTATION 270
/* The back JSON's RAW16: RGGB, black level 64, white level 4000 */
#define RAW_BLACK 64
#define RAW_WHITE 4000

struct chars_head {
    size_t width, height;
};

/* EmulatedSensor's YCbCrPlanes (passed by value: by address, as it is larger than 16 bytes) */
struct planes {
    uint8_t *y, *cb, *cr;
    uint32_t y_stride, cbcr_stride, cbcr_step;
    size_t bytes_per_pixel;
};

enum { kLayoutRGB = 0, kLayoutRGBA = 1, kLayoutARGB = 2 };

/* The connection is made and dropped by the worker thread: the HAL's
 * capture threads never wait for the phone's camera (cameraserver's
 * watchdog aborts it when a HAL call stalls). */
static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int fd;
    int facing; /* connected: 0 back, 1 front, -1 none */
    int want;   /* asked for by the last capture */
    struct vine_camera_shm* shm;
    size_t shm_size;
    int64_t last_use_ns;
    bool worker;
} g = {.lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER, .fd = -1, .facing = -1, .want = -1};

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void disconnect_locked(void)
{
    if (g.shm) munmap(g.shm, g.shm_size);
    g.shm = NULL;
    if (g.fd >= 0) close(g.fd);
    g.fd = -1;
    g.facing = -1;
}

/* The phone's camera facing that way: its socket and frames, or false */
static bool open_host(int facing, int* out_fd, struct vine_camera_shm** out_shm, size_t* out_size)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    memcpy(sa.sun_path, kHost, sizeof kHost);
    const char* req = facing ? "open front\n" : "open back\n";
    if (connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0 || write(fd, req, strlen(req)) != (ssize_t)strlen(req)) {
        close(fd);
        return false;
    }
    /* The phone's camera takes a moment to open */
    struct timeval tv = {.tv_sec = 5};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char reply[64] = {0};
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {.iov_base = reply, .iov_len = sizeof reply - 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
    const ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    int shm_fd = -1;
    struct cmsghdr* c = n > 0 ? CMSG_FIRSTHDR(&msg) : NULL;
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&shm_fd, CMSG_DATA(c), sizeof shm_fd);
    unsigned w, h, o;
    if (n <= 0 || shm_fd < 0 || sscanf(reply, "ok %u %u %u", &w, &h, &o) != 3) {
        if (shm_fd >= 0) close(shm_fd);
        close(fd);
        return false;
    }
    const size_t size = sizeof(struct vine_camera_shm) + (size_t)w * h * 3 / 2 * VINE_CAMERA_SLOTS;
    void* p = mmap(NULL, size, PROT_READ, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    if (p == MAP_FAILED || ((struct vine_camera_shm*)p)->magic != VINE_CAMERA_MAGIC ||
        ((struct vine_camera_shm*)p)->width != w || ((struct vine_camera_shm*)p)->height != h) {
        if (p != MAP_FAILED) munmap(p, size);
        close(fd);
        return false;
    }
    *out_fd = fd;
    *out_shm = p;
    *out_size = size;
    return true;
}

/* Connects to the camera the captures ask for; disconnects a second
 * after the last capture (the phone's camera stops) */
static void* worker(void* arg)
{
    (void)arg;
    int64_t retry_after = 0;
    pthread_mutex_lock(&g.lock);
    for (;;) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 1;
        pthread_cond_timedwait(&g.wake, &g.lock, &t);
        const int64_t now = now_ns();
        if (g.fd >= 0 && now - g.last_use_ns > 1000000000LL) {
            disconnect_locked();
            g.want = -1;
        }
        if (g.want < 0 || g.want == g.facing || now < retry_after) continue;
        const int want = g.want;
        disconnect_locked();
        pthread_mutex_unlock(&g.lock);
        int fd;
        struct vine_camera_shm* shm;
        size_t size;
        const bool ok = open_host(want, &fd, &shm, &size);
        pthread_mutex_lock(&g.lock);
        if (!ok) {
            retry_after = now_ns() + 3000000000LL;
            continue;
        }
        g.fd = fd;
        g.shm = shm;
        g.shm_size = size;
        g.facing = want;
    }
    return NULL;
}

/* The phone's latest frame for that camera, or NULL (the test scene then).
 * Holds g.lock when it returns one: release_frame() after sampling. */
static const struct vine_camera_shm* acquire_frame(const void* chars, const uint8_t** i420)
{
    const struct chars_head* ch = chars;
    int facing;
    if (ch->width == BACK_WIDTH) facing = 0;
    else if (ch->width == FRONT_WIDTH) facing = 1;
    else return NULL;
    pthread_mutex_lock(&g.lock);
    g.last_use_ns = now_ns();
    if (!g.worker) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) {
            pthread_detach(t);
            g.worker = true;
        }
    }
    if (g.want != facing) {
        g.want = facing;
        pthread_cond_signal(&g.wake);
    }
    struct vine_camera_shm* s = g.facing == facing ? g.shm : NULL;
    if (!s || !atomic_load(&s->frames)) {
        pthread_mutex_unlock(&g.lock);
        return NULL;
    }
    const uint32_t slot = atomic_load(&s->latest) % VINE_CAMERA_SLOTS;
    *i420 = (const uint8_t*)s + s->slot_offset + (size_t)slot * s->slot_size;
    return s;
}

static void release_frame(void)
{
    pthread_mutex_unlock(&g.lock);
}

/* Where output pixel (x, y) of a w x h output samples the phone's frame:
 * hx = xa[x] + ya[y] style maps, split by axis. The output shows the
 * sensor's field of view cropped to its aspect, then by the zoom; the
 * phone's frame is turned by the difference of the two sensor
 * orientations and cropped to the guest sensor's aspect. */
struct sampler {
    uint32_t* col; /* per output column: its source coordinate */
    uint32_t* row; /* per output row */
    bool swap;     /* col indexes the phone frame's rows (a quarter turn) */
    uint32_t fw, fh;
};

static bool make_sampler(struct sampler* s, const struct vine_camera_shm* f, uint32_t w, uint32_t h,
                         const struct chars_head* ch, float zoom, int guest_orientation)
{
    s->fw = f->width;
    s->fh = f->height;
    s->col = malloc(sizeof(uint32_t) * w);
    s->row = malloc(sizeof(uint32_t) * h);
    if (!s->col || !s->row) {
        free(s->col);
        free(s->row);
        return false;
    }
    if (zoom < 1.0f) zoom = 1.0f;
    const int turn = ((((int)f->orientation - guest_orientation) % 360 + 360) % 360) / 90;
    s->swap = turn & 1;
    /* The guest sensor's view, in [0,1] of its width and height */
    const double sensor_aspect = (double)ch->width / ch->height, out_aspect = (double)w / h;
    double vw = 1.0, vh = 1.0;
    if (out_aspect > sensor_aspect) vh = sensor_aspect / out_aspect;
    else vw = out_aspect / sensor_aspect;
    vw /= zoom;
    vh /= zoom;
    /* The phone's frame turned to the guest sensor's orientation, cropped to its aspect */
    const double turned_aspect = s->swap ? (double)f->height / f->width : (double)f->width / f->height;
    double cw = 1.0, ch2 = 1.0;
    if (turned_aspect > sensor_aspect) cw = sensor_aspect / turned_aspect;
    else ch2 = turned_aspect / sensor_aspect;
    for (int axis = 0; axis < 2; axis++) {
        const uint32_t n = axis ? h : w;
        uint32_t* out = axis ? s->row : s->col;
        const double view = axis ? vh : vw, crop = axis ? ch2 : cw;
        for (uint32_t i = 0; i < n; i++) {
            /* u: the guest sensor's coordinate along this axis, then the turned frame's */
            double u = 0.5 + ((i + 0.5) / n - 0.5) * view;
            u = 0.5 + (u - 0.5) * crop;
            /* Turned frame (tu, tv) to the phone frame: turn 1 (90): (tv, 1-tu);
             * 2: (1-tu, 1-tv); 3: (1-tv, tu). This axis's coordinate lands on
             * the phone frame's x (axis 0, turns 0 and 2; axis 1, turns 1 and 3)
             * or y, mirrored where 1-u. */
            bool mirror;
            if (turn == 0) mirror = false;
            else if (turn == 2) mirror = true;
            else if (turn == 1) mirror = axis == 0;
            else mirror = axis == 1;
            if (mirror) u = 1.0 - u;
            const bool to_x = (axis == 0) != s->swap;
            const uint32_t size = to_x ? f->width : f->height;
            long v = (long)(u * size);
            if (v < 0) v = 0;
            if (v >= (long)size) v = (long)size - 1;
            out[i] = (uint32_t)v;
        }
    }
    return true;
}

static void free_sampler(struct sampler* s)
{
    free(s->col);
    free(s->row);
}

static inline void source_xy(const struct sampler* s, uint32_t x, uint32_t y, uint32_t* fx, uint32_t* fy)
{
    if (s->swap) {
        *fx = s->row[y];
        *fy = s->col[x];
    } else {
        *fx = s->col[x];
        *fy = s->row[y];
    }
}

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static inline void yuv_at(const uint8_t* i420, const struct sampler* s, uint32_t fx, uint32_t fy, int* yy, int* u,
                          int* v)
{
    const uint32_t fw = s->fw, fh = s->fh;
    *yy = i420[(size_t)fy * fw + fx];
    const size_t c = (size_t)(fy / 2) * (fw / 2) + fx / 2;
    *u = i420[(size_t)fw * fh + c];
    *v = i420[(size_t)fw * fh + (size_t)(fw / 2) * (fh / 2) + c];
}

/* JFIF YCbCr (what camera YUV is) to RGB */
static inline void to_rgb(int y, int u, int v, uint8_t* r, uint8_t* g_, uint8_t* b)
{
    u -= 128;
    v -= 128;
    *r = clamp8(y + ((91881 * v) >> 16));
    *g_ = clamp8(y - ((22554 * u + 46802 * v) >> 16));
    *b = clamp8(y + ((116130 * u) >> 16));
}

static int guest_orientation(const struct chars_head* ch)
{
    return ch->width == FRONT_WIDTH ? FRONT_ORIENTATION : BACK_ORIENTATION;
}

/* ---- the interposed EmulatedSensor methods ---- */

typedef void (*yuv_fn)(void*, const struct planes*, uint32_t, uint32_t, uint32_t, float, bool, const void*);
typedef void (*rgb_fn)(void*, uint8_t*, uint32_t, uint32_t, uint32_t, int, uint32_t, const void*);
typedef void (*raw_fn)(void*, uint8_t*, size_t, uint32_t, const void*);

#define SYM_YUV "_ZN7android14EmulatedSensor13CaptureYUV420ENS_11YCbCrPlanesEjjjfbRKNS_21SensorCharacteristicsE"
#define SYM_RGB "_ZN7android14EmulatedSensor10CaptureRGBEPhjjjNS0_9RGBLayoutEjRKNS_21SensorCharacteristicsE"
#define SYM_RAW "_ZN7android14EmulatedSensor16CaptureRawBinnedEPhmjRKNS_21SensorCharacteristicsE"

EXPORT void _ZN7android14EmulatedSensor13CaptureYUV420ENS_11YCbCrPlanesEjjjfbRKNS_21SensorCharacteristicsE(
    void* self, const struct planes* p, uint32_t w, uint32_t h, uint32_t gain, float zoom, bool rotate,
    const void* chars)
{
    static yuv_fn real;
    if (!real) real = (yuv_fn)dlsym(RTLD_NEXT, SYM_YUV);
    const uint8_t* i420 = NULL;
    const struct vine_camera_shm* f =
        p->bytes_per_pixel == 1 && !rotate ? acquire_frame(chars, &i420) : NULL;
    struct sampler s;
    if (!f) goto real_capture;
    if (!make_sampler(&s, f, w, h, chars, zoom, guest_orientation(chars))) {
        release_frame();
        goto real_capture;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* out = p->y + (size_t)y * p->y_stride;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t fx, fy;
            source_xy(&s, x, y, &fx, &fy);
            out[x] = i420[(size_t)fy * s.fw + fx];
        }
    }
    for (uint32_t y = 0; y < h / 2; y++) {
        uint8_t* cb = p->cb + (size_t)y * p->cbcr_stride;
        uint8_t* cr = p->cr + (size_t)y * p->cbcr_stride;
        for (uint32_t x = 0; x < w / 2; x++) {
            uint32_t fx, fy;
            source_xy(&s, x * 2, y * 2, &fx, &fy);
            int yy, u, v;
            yuv_at(i420, &s, fx, fy, &yy, &u, &v);
            cb[(size_t)x * p->cbcr_step] = (uint8_t)u;
            cr[(size_t)x * p->cbcr_step] = (uint8_t)v;
        }
    }
    free_sampler(&s);
    release_frame();
    return;
real_capture:
    if (real) real(self, p, w, h, gain, zoom, rotate, chars);
}

EXPORT void _ZN7android14EmulatedSensor10CaptureRGBEPhjjjNS0_9RGBLayoutEjRKNS_21SensorCharacteristicsE(
    void* self, uint8_t* img, uint32_t w, uint32_t h, uint32_t stride, int layout, uint32_t gain, const void* chars)
{
    static rgb_fn real;
    if (!real) real = (rgb_fn)dlsym(RTLD_NEXT, SYM_RGB);
    const uint8_t* i420 = NULL;
    const struct vine_camera_shm* f = layout >= kLayoutRGB && layout <= kLayoutARGB ? acquire_frame(chars, &i420) : NULL;
    struct sampler s;
    if (!f) goto real_capture;
    if (!make_sampler(&s, f, w, h, chars, 1.0f, guest_orientation(chars))) {
        release_frame();
        goto real_capture;
    }
    const uint32_t bpp = layout == kLayoutRGB ? 3 : 4;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* out = img + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++, out += bpp) {
            uint32_t fx, fy;
            source_xy(&s, x, y, &fx, &fy);
            int yy, u, v;
            yuv_at(i420, &s, fx, fy, &yy, &u, &v);
            uint8_t r, gg, b;
            to_rgb(yy, u, v, &r, &gg, &b);
            if (layout == kLayoutARGB) {
                out[0] = 255; out[1] = r; out[2] = gg; out[3] = b;
            } else {
                out[0] = r; out[1] = gg; out[2] = b;
                if (layout == kLayoutRGBA) out[3] = 255;
            }
        }
    }
    free_sampler(&s);
    release_frame();
    return;
real_capture:
    if (real) real(self, img, w, h, stride, layout, gain, chars);
}

/* RAW16 RGGB over the whole sensor: the frame's sRGB values made linear */
EXPORT void _ZN7android14EmulatedSensor16CaptureRawBinnedEPhmjRKNS_21SensorCharacteristicsE(
    void* self, uint8_t* img, size_t row_stride, uint32_t gain, const void* chars)
{
    static raw_fn real;
    if (!real) real = (raw_fn)dlsym(RTLD_NEXT, SYM_RAW);
    static uint16_t lut[256];
    static bool lut_ready;
    if (!lut_ready) {
        for (int i = 0; i < 256; i++) {
            const double c = i / 255.0;
            const double lin = c <= 0.04045 ? c / 12.92 : __builtin_pow((c + 0.055) / 1.055, 2.4);
            lut[i] = (uint16_t)(RAW_BLACK + lin * (RAW_WHITE - RAW_BLACK) + 0.5);
        }
        lut_ready = true;
    }
    const struct chars_head* ch = chars;
    const uint8_t* i420 = NULL;
    const struct vine_camera_shm* f = acquire_frame(chars, &i420);
    struct sampler s;
    if (!f) goto real_capture;
    const uint32_t w = (uint32_t)ch->width, h = (uint32_t)ch->height;
    if (!make_sampler(&s, f, w, h, ch, 1.0f, guest_orientation(ch))) {
        release_frame();
        goto real_capture;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint16_t* out = (uint16_t*)(img + (size_t)y * row_stride);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t fx, fy;
            source_xy(&s, x, y, &fx, &fy);
            int yy, u, v;
            yuv_at(i420, &s, fx, fy, &yy, &u, &v);
            uint8_t rgb[3];
            to_rgb(yy, u, v, &rgb[0], &rgb[1], &rgb[2]);
            /* R G / G B */
            const int c = (y & 1) ? ((x & 1) ? 2 : 1) : ((x & 1) ? 1 : 0);
            out[x] = lut[rgb[c]];
        }
    }
    free_sampler(&s);
    release_frame();
    return;
real_capture:
    if (real) real(self, img, row_stride, gain, chars);
}
