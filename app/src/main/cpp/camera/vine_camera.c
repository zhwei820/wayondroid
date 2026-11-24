/* vine_camera --socket PATH
 *
 * The phone's cameras for a rootless guest. The guest's camera HAL (the
 * emulator's, with camera_shim.c preloaded) connects to PATH while one of
 * its cameras streams and asks for the phone's camera facing the same way:
 *
 *   guest: "open back\n" | "open front\n"
 *   here:  "ok WIDTH HEIGHT ORIENTATION\n", with a memfd (SCM_RIGHTS)
 *          laid out as struct vine_camera_shm (camera_shm.h), or "err\n"
 *
 * The camera runs, its frames copied into the memfd as I420, until the
 * guest closes the connection. One guest connection at a time; a new one
 * replaces the old. The phone only lets an app use its camera while the
 * app is in the foreground (and it holds CAMERA). */
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <errno.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include "camera_shm.h"

/* The largest frame copied: a phone's preview-sized 4:3 stream */
#define MAX_WIDTH 1920
#define MAX_HEIGHT 1440

static struct {
    ACameraManager* manager;
    ACameraDevice* device;
    AImageReader* reader;
    ACaptureSessionOutputContainer* outputs;
    ACaptureSessionOutput* output;
    ACameraOutputTarget* target;
    ACaptureRequest* request;
    ACameraCaptureSession* session;
    struct vine_camera_shm* shm;
    size_t shm_size;
    int shm_fd;
    int client;
} g = {.shm_fd = -1, .client = -1};

static void logmsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "vine_camera: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---- frames ---- */

static void on_image(void* context, AImageReader* reader)
{
    (void)context;
    AImage* image = NULL;
    if (AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK || !image) return;
    struct vine_camera_shm* s = g.shm;
    int32_t w = 0, h = 0;
    AImage_getWidth(image, &w);
    AImage_getHeight(image, &h);
    if (s && w == (int32_t)s->width && h == (int32_t)s->height) {
        const uint32_t slot = (atomic_load(&s->latest) + 1) % VINE_CAMERA_SLOTS;
        uint8_t* dst = (uint8_t*)s + s->slot_offset + (size_t)slot * s->slot_size;
        for (int p = 0; p < 3; p++) {
            uint8_t* data = NULL;
            int len = 0, row = 0, step = 1;
            if (AImage_getPlaneData(image, p, &data, &len) != AMEDIA_OK || !data) goto out;
            AImage_getPlaneRowStride(image, p, &row);
            AImage_getPlanePixelStride(image, p, &step);
            const int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
            for (int y = 0; y < ph; y++) {
                const uint8_t* src = data + (size_t)y * row;
                if (step == 1) {
                    memcpy(dst, src, (size_t)pw);
                } else {
                    for (int x = 0; x < pw; x++) dst[x] = src[x * step];
                }
                dst += pw;
            }
        }
        atomic_fetch_add(&s->frames, 1);
        atomic_store(&s->latest, slot);
    }
out:
    AImage_delete(image);
}

/* ---- the camera ---- */

static void on_disconnected(void* context, ACameraDevice* device)
{
    (void)context;
    (void)device;
    logmsg("camera disconnected");
}

static void on_error(void* context, ACameraDevice* device, int error)
{
    (void)context;
    (void)device;
    logmsg("camera error %d", error);
}

static void session_closed(void* context, ACameraCaptureSession* session)
{
    (void)context;
    (void)session;
}

static void stop_camera(void)
{
    if (g.session) {
        ACameraCaptureSession_stopRepeating(g.session);
        ACameraCaptureSession_close(g.session);
        g.session = NULL;
    }
    if (g.request) ACaptureRequest_free(g.request), g.request = NULL;
    if (g.target) ACameraOutputTarget_free(g.target), g.target = NULL;
    if (g.outputs) ACaptureSessionOutputContainer_free(g.outputs), g.outputs = NULL;
    if (g.output) ACaptureSessionOutput_free(g.output), g.output = NULL;
    if (g.device) ACameraDevice_close(g.device), g.device = NULL;
    if (g.reader) AImageReader_delete(g.reader), g.reader = NULL;
    if (g.shm) munmap(g.shm, g.shm_size), g.shm = NULL;
    if (g.shm_fd >= 0) close(g.shm_fd), g.shm_fd = -1;
}

/* The phone's camera facing that way (ACAMERA_LENS_FACING_*), and the
 * largest YUV size within MAX_WIDTH x MAX_HEIGHT, 4:3 if it has one */
static bool pick_camera(uint8_t facing, char* id, size_t id_size, int32_t* width, int32_t* height,
                        int32_t* orientation)
{
    ACameraIdList* ids = NULL;
    if (ACameraManager_getCameraIdList(g.manager, &ids) != ACAMERA_OK || !ids) return false;
    bool found = false;
    for (int i = 0; i < ids->numCameras && !found; i++) {
        ACameraMetadata* m = NULL;
        if (ACameraManager_getCameraCharacteristics(g.manager, ids->cameraIds[i], &m) != ACAMERA_OK || !m) continue;
        ACameraMetadata_const_entry e = {0};
        if (ACameraMetadata_getConstEntry(m, ACAMERA_LENS_FACING, &e) == ACAMERA_OK && e.count &&
            e.data.u8[0] == facing &&
            ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &e) == ACAMERA_OK) {
            int32_t best_w = 0, best_h = 0;
            bool best_43 = false;
            for (uint32_t k = 0; k + 3 < e.count; k += 4) {
                const int32_t* c = e.data.i32 + k;
                if (c[0] != AIMAGE_FORMAT_YUV_420_888 || c[3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT ||
                    c[1] > MAX_WIDTH || c[2] > MAX_HEIGHT || (c[1] & 1) || (c[2] & 1))
                    continue;
                const bool is_43 = c[1] * 3 == c[2] * 4;
                if ((is_43 && !best_43) || (is_43 == best_43 && c[1] * c[2] > best_w * best_h)) {
                    best_w = c[1];
                    best_h = c[2];
                    best_43 = is_43;
                }
            }
            if (best_w) {
                *width = best_w;
                *height = best_h;
                *orientation = 0;
                if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_ORIENTATION, &e) == ACAMERA_OK && e.count)
                    *orientation = e.data.i32[0];
                snprintf(id, id_size, "%s", ids->cameraIds[i]);
                found = true;
            }
        }
        ACameraMetadata_free(m);
    }
    ACameraManager_deleteCameraIdList(ids);
    return found;
}

static bool start_camera(uint8_t facing, int32_t* width, int32_t* height, int32_t* orientation)
{
    char id[64];
    if (!pick_camera(facing, id, sizeof id, width, height, orientation)) {
        logmsg("no %s camera", facing == ACAMERA_LENS_FACING_FRONT ? "front" : "back");
        return false;
    }
    const size_t slot = (size_t)*width * *height * 3 / 2;
    g.shm_size = sizeof(struct vine_camera_shm) + slot * VINE_CAMERA_SLOTS;
    g.shm_fd = (int)syscall(__NR_memfd_create, "vine-camera", 1u /* MFD_CLOEXEC */);
    if (g.shm_fd < 0 || ftruncate(g.shm_fd, (off_t)g.shm_size) != 0) goto fail;
    g.shm = mmap(NULL, g.shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, g.shm_fd, 0);
    if (g.shm == MAP_FAILED) {
        g.shm = NULL;
        goto fail;
    }
    g.shm->magic = VINE_CAMERA_MAGIC;
    g.shm->width = (uint32_t)*width;
    g.shm->height = (uint32_t)*height;
    g.shm->orientation = (uint32_t)*orientation;
    g.shm->slot_offset = sizeof(struct vine_camera_shm);
    g.shm->slot_size = (uint32_t)slot;
    atomic_store(&g.shm->latest, 0);
    atomic_store(&g.shm->frames, 0);

    if (AImageReader_new(*width, *height, AIMAGE_FORMAT_YUV_420_888, 4, &g.reader) != AMEDIA_OK) goto fail;
    AImageReader_ImageListener listener = {.context = NULL, .onImageAvailable = on_image};
    AImageReader_setImageListener(g.reader, &listener);
    ANativeWindow* window = NULL;
    if (AImageReader_getWindow(g.reader, &window) != AMEDIA_OK) goto fail;
    static ACameraDevice_StateCallbacks device_cb = {.onDisconnected = on_disconnected, .onError = on_error};
    camera_status_t st = ACameraManager_openCamera(g.manager, id, &device_cb, &g.device);
    if (st != ACAMERA_OK) {
        logmsg("cannot open camera %s: %d", id, st);
        goto fail;
    }
    static ACameraCaptureSession_stateCallbacks session_cb = {.onClosed = session_closed};
    if (ACaptureSessionOutputContainer_create(&g.outputs) != ACAMERA_OK ||
        ACaptureSessionOutput_create(window, &g.output) != ACAMERA_OK ||
        ACaptureSessionOutputContainer_add(g.outputs, g.output) != ACAMERA_OK ||
        ACameraOutputTarget_create(window, &g.target) != ACAMERA_OK ||
        ACameraDevice_createCaptureRequest(g.device, TEMPLATE_PREVIEW, &g.request) != ACAMERA_OK ||
        ACaptureRequest_addTarget(g.request, g.target) != ACAMERA_OK ||
        ACameraDevice_createCaptureSession(g.device, g.outputs, &session_cb, &g.session) != ACAMERA_OK ||
        ACameraCaptureSession_setRepeatingRequest(g.session, NULL, 1, &g.request, NULL) != ACAMERA_OK) {
        logmsg("cannot start camera %s", id);
        goto fail;
    }
    logmsg("camera %s streaming %dx%d, orientation %d", id, *width, *height, *orientation);
    return true;
fail:
    stop_camera();
    return false;
}

/* ---- the guest ---- */

static void drop_client(void)
{
    if (g.client < 0) return;
    close(g.client);
    g.client = -1;
    stop_camera();
    logmsg("guest closed the camera");
}

static void send_reply(const char* text, int fd)
{
    struct iovec iov = {.iov_base = (void*)text, .iov_len = strlen(text)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
    char control[CMSG_SPACE(sizeof(int))];
    if (fd >= 0) {
        memset(control, 0, sizeof control);
        msg.msg_control = control;
        msg.msg_controllen = sizeof control;
        struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof fd);
    }
    sendmsg(g.client, &msg, MSG_NOSIGNAL);
}

static void on_request(const char* line)
{
    uint8_t facing;
    if (!strcmp(line, "open back")) facing = ACAMERA_LENS_FACING_BACK;
    else if (!strcmp(line, "open front")) facing = ACAMERA_LENS_FACING_FRONT;
    else return send_reply("err\n", -1);
    stop_camera();
    int32_t w, h, o;
    if (!start_camera(facing, &w, &h, &o)) return send_reply("err\n", -1);
    char reply[64];
    snprintf(reply, sizeof reply, "ok %d %d %d\n", w, h, o);
    send_reply(reply, g.shm_fd);
}

static int listen_on(const char* path)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof sa.sun_path) return -1;
    strcpy(sa.sun_path, path);
    unlink(path);
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    if (bind(s, (struct sockaddr*)&sa, sizeof sa) != 0 || listen(s, 2) != 0) {
        close(s);
        return -1;
    }
    /* The guest's camera HAL runs as another guest uid, but the same host uid */
    chmod(path, 0666);
    return s;
}

int main(int argc, char** argv)
{
    const char* path = NULL;
    for (int i = 1; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--socket")) path = argv[i + 1];
    if (!path) {
        fprintf(stderr, "usage: vine_camera --socket PATH\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    g.manager = ACameraManager_create();
    if (!g.manager) {
        logmsg("no camera manager");
        return 1;
    }
    const int s = listen_on(path);
    if (s < 0) {
        logmsg("cannot listen on %s", path);
        return 1;
    }
    char in[128];
    size_t in_len = 0;
    for (;;) {
        struct pollfd p[2] = {{.fd = s, .events = POLLIN}, {.fd = g.client, .events = POLLIN}};
        if (poll(p, g.client >= 0 ? 2 : 1, -1) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (p[0].revents & POLLIN) {
            const int c = accept4(s, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                drop_client();
                g.client = c;
                in_len = 0;
            }
            continue;
        }
        if (g.client < 0 || !(p[1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        const ssize_t n = recv(g.client, in + in_len, sizeof in - 1 - in_len, 0);
        if (n <= 0) {
            drop_client();
            continue;
        }
        in_len += (size_t)n;
        in[in_len] = 0;
        char* nl;
        while ((nl = strchr(in, '\n'))) {
            *nl = 0;
            on_request(in);
            in_len -= (size_t)(nl + 1 - in);
            memmove(in, nl + 1, in_len + 1);
        }
        if (in_len == sizeof in - 1) drop_client();
    }
}
