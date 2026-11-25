/* vine_sensors --socket PATH [--package NAME]
 *
 * The phone's sensors for a rootless guest. The guest runs the emulator's
 * sensors HAL (android.hardware.sensors@2.1 multihal with the goldfish
 * "ranchu" sub-HAL), whose /dev/goldfish_pipe libvine_sensors_shim turns
 * into a connection to PATH. One guest at a time; a new connection
 * replaces the old one (the HAL service restarted).
 *
 * The wire is the emulator's "qemud:sensors" channel: the guest first
 * writes "pipe:qemud:sensors\0", then both sides send frames of a 4-digit
 * hex length and the text. The guest sends "list-sensors" (answered with
 * the decimal mask of the sensors there are, bit = goldfish sensor
 * handle), "set:NAME:0|1" and "set-delay:MS"; we send "NAME:v[:v:v]"
 * events. Handles, names and value formats follow the goldfish sub-HAL
 * (device/generic/goldfish/sensors, android14 branch): set names and
 * event names differ for the magnetic field.
 *
 * Continuous sensors only deliver while the app's uid is in the
 * foreground (SensorService's policy for idle uids).
 *
 * A phone without a barometer (the guest's Pixel 5 has one) gets a
 * made-up one: sea-level-ish pressure that drifts slowly, with sensor noise.
 *
 * The step counter, step detector and significant motion sensors (the
 * Pixel 5's, which an app could only read from the phone with
 * ACTIVITY_RECOGNITION) come from a pedometer on the phone's
 * accelerometer. They go to the guest as the sub-HAL's three hinge
 * angles, which the guest's sensors_shim.c retypes. */
#include <android/looper.h>
#include <android/sensor.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define LOOPER_ID_SENSORS 1
#define MIN_DELAY_MS 5   /* 200 Hz, the most an app gets without HIGH_SAMPLING_RATE_SENSORS */

struct map {
    int bit;              /* goldfish sensor handle */
    int type;             /* ASENSOR_TYPE_* */
    const char* set_name; /* in set:NAME:x */
    const char* event;    /* event prefix */
    int values;           /* 1 or 3 */
};

static const struct map kMap[] = {
    {0, ASENSOR_TYPE_ACCELEROMETER, "acceleration", "acceleration", 3},
    {1, ASENSOR_TYPE_GYROSCOPE, "gyroscope", "gyroscope", 3},
    {2, ASENSOR_TYPE_MAGNETIC_FIELD, "magnetic-field", "magnetic", 3},
    {4, ASENSOR_TYPE_AMBIENT_TEMPERATURE, "temperature", "temperature", 1},
    {5, ASENSOR_TYPE_PROXIMITY, "proximity", "proximity", 1},
    {6, ASENSOR_TYPE_LIGHT, "light", "light", 1},
    {7, ASENSOR_TYPE_PRESSURE, "pressure", "pressure", 1},
    {8, ASENSOR_TYPE_RELATIVE_HUMIDITY, "humidity", "humidity", 1},
    {9, ASENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED, "magnetic-field-uncalibrated", "magnetic-uncalibrated", 3},
    {10, ASENSOR_TYPE_GYROSCOPE_UNCALIBRATED, "gyroscope-uncalibrated", "gyroscope-uncalibrated", 3},
    {11, ASENSOR_TYPE_STEP_COUNTER, "hinge-angle0", "hinge-angle0", 1},
    {12, ASENSOR_TYPE_STEP_DETECTOR, "hinge-angle1", "hinge-angle1", 1},
    {13, ASENSOR_TYPE_SIGNIFICANT_MOTION, "hinge-angle2", "hinge-angle2", 1},
    {17, ASENSOR_TYPE_ACCELEROMETER_UNCALIBRATED, "acceleration-uncalibrated", "acceleration-uncalibrated", 3},
};
#define NMAP (sizeof kMap / sizeof kMap[0])
#define ACCEL 0          /* kMap index of the accelerometer */
#define PEDO_MS 20       /* the pedometer's accelerometer rate, 50 Hz */
#define SMD_STEPS 8      /* significant motion: this many steps ... */
#define SMD_WINDOW 10.0  /* ... within this many seconds */

static struct {
    ASensorManager* manager;
    ASensorEventQueue* queue;
    ALooper* looper;
    const ASensor* sensor[NMAP];
    bool enabled[NMAP];
    int delay_ms;
    int client;
    bool synth_pressure; /* no phone barometer: kMap's pressure is made up */
    int synth_timer;
    double synth_base;
    bool hello_done;
    char in[512];
    size_t in_len;
    bool accel_on; /* the phone's accelerometer, for the guest's or the pedometer */
    struct {
        double mean, smooth, last_step, recent[SMD_STEPS];
        bool armed, smd_fired;
        unsigned steps, recent_n, smd_seq;
    } pedo;
} g = {.delay_ms = 200, .client = -1, .synth_timer = -1};

static bool pedometer(size_t i)
{
    return kMap[i].type == ASENSOR_TYPE_STEP_COUNTER || kMap[i].type == ASENSOR_TYPE_STEP_DETECTOR ||
           kMap[i].type == ASENSOR_TYPE_SIGNIFICANT_MOTION;
}

static bool pedometer_on(void)
{
    for (size_t i = 0; i < NMAP; i++)
        if (pedometer(i) && g.enabled[i]) return true;
    return false;
}

static bool synthetic(size_t i)
{
    if (pedometer(i)) return g.sensor[ACCEL] != NULL;
    return !g.sensor[i] && g.synth_pressure && kMap[i].type == ASENSOR_TYPE_PRESSURE;
}

/* The made-up barometer's period: the guest's rate, at most 25 Hz as a phone's */
static void arm_synthetic(bool on)
{
    if (g.synth_timer < 0) return;
    int ms = g.delay_ms < 40 ? 40 : g.delay_ms;
    struct itimerspec t = {0};
    if (on) {
        t.it_interval.tv_sec = ms / 1000;
        t.it_interval.tv_nsec = (long)(ms % 1000) * 1000000L;
        t.it_value = t.it_interval;
    }
    timerfd_settime(g.synth_timer, 0, &t, NULL);
}

static void logmsg(const char* fmt, const char* a)
{
    fprintf(stderr, "vine_sensors: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
}

/* The guest's sensors_shim.c makes wake-up twins of these in the unused
 * handles (base handle, twin handle); they need no events of their own */
static const int kWakeTwins[][2] = {{0, 3}, {1, 4}, {2, 8}, {6, 14}, {7, 15}, {11, 16}};

static uint32_t mask(void)
{
    uint32_t m = 0;
    for (size_t i = 0; i < NMAP; i++)
        if (g.sensor[i] || synthetic(i)) m |= 1u << kMap[i].bit;
    for (size_t i = 0; i < sizeof kWakeTwins / sizeof kWakeTwins[0]; i++)
        if (m & (1u << kWakeTwins[i][0])) m |= 1u << kWakeTwins[i][1];
    return m;
}

/* Frames are small; a guest that stops reading loses events instead of
 * stalling the loop. */
static void send_frame(const char* text)
{
    if (g.client < 0) return;
    char buf[160];
    const int len = (int)strlen(text);
    const int n = snprintf(buf, sizeof buf, "%04x%s", len, text);
    if (n <= 0 || n >= (int)sizeof buf) return;
    send(g.client, buf, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void apply_rate(size_t i)
{
    int ms = g.delay_ms;
    if (i == ACCEL && pedometer_on() && (!g.enabled[ACCEL] || ms > PEDO_MS)) ms = PEDO_MS;
    int us = (ms < MIN_DELAY_MS ? MIN_DELAY_MS : ms) * 1000;
    const int min_us = ASensor_getMinDelay(g.sensor[i]);
    if (min_us > 0 && us < min_us) us = min_us;
    ASensorEventQueue_setEventRate(g.queue, g.sensor[i], us);
}

/* The phone's accelerometer runs while the guest's or the pedometer wants it */
static void sync_accel(void)
{
    const bool want = g.enabled[ACCEL] || pedometer_on();
    if (want != g.accel_on) {
        if (want && ASensorEventQueue_enableSensor(g.queue, g.sensor[ACCEL]) < 0) {
            logmsg("cannot enable %s", kMap[ACCEL].set_name);
            return;
        }
        if (!want) ASensorEventQueue_disableSensor(g.queue, g.sensor[ACCEL]);
        g.accel_on = want;
    }
    if (want) apply_rate(ACCEL);
}

static void enable(size_t i, bool on)
{
    if (pedometer(i)) {
        if (!synthetic(i)) return;
        g.enabled[i] = on;
        if (on && kMap[i].type == ASENSOR_TYPE_SIGNIFICANT_MOTION) g.pedo.smd_fired = false;
        sync_accel();
        return;
    }
    if (i == ACCEL) {
        if (!g.sensor[i]) return;
        g.enabled[i] = on;
        sync_accel();
        return;
    }
    if (synthetic(i)) {
        g.enabled[i] = on;
        arm_synthetic(on);
        return;
    }
    if (!g.sensor[i] || g.enabled[i] == on) return;
    if (on) {
        if (ASensorEventQueue_enableSensor(g.queue, g.sensor[i]) < 0) {
            logmsg("cannot enable %s", kMap[i].set_name);
            return;
        }
        apply_rate(i);
    } else {
        ASensorEventQueue_disableSensor(g.queue, g.sensor[i]);
    }
    g.enabled[i] = on;
}

static void disable_all(void)
{
    for (size_t i = 0; i < NMAP; i++) enable(i, false);
    g.pedo.steps = 0; /* a new guest boot */
}

static void drop_client(void)
{
    if (g.client < 0) return;
    ALooper_removeFd(g.looper, g.client);
    close(g.client);
    g.client = -1;
    disable_all();
    logmsg("%s", "guest gone");
}

static void handle_command(const char* msg)
{
    if (strcmp(msg, "list-sensors") == 0) {
        char m[16];
        snprintf(m, sizeof m, "%u", mask());
        send_frame(m);
        return;
    }
    int delay;
    if (sscanf(msg, "set-delay:%d", &delay) == 1) {
        g.delay_ms = delay;
        for (size_t i = 0; i < NMAP; i++)
            if (g.enabled[i] && !pedometer(i)) synthetic(i) ? arm_synthetic(true) : apply_rate(i);
        sync_accel();
        return;
    }
    if (strncmp(msg, "set:", 4) == 0) {
        const char* name = msg + 4;
        const char* colon = strrchr(name, ':');
        if (!colon) return;
        for (size_t i = 0; i < NMAP; i++) {
            if (strlen(kMap[i].set_name) == (size_t)(colon - name) && !strncmp(kMap[i].set_name, name, colon - name)) {
                enable(i, colon[1] == '1');
                return;
            }
        }
        return;
    }
    /* "time:NS" (the guest's clock) needs no answer: the HAL stamps
     * events with its own clock */
}

/* Consumes the hello, then every complete frame in g.in */
static void parse_input(void)
{
    size_t pos = 0;
    if (!g.hello_done) {
        char* nul = memchr(g.in, 0, g.in_len);
        if (!nul) {
            if (g.in_len == sizeof g.in) drop_client();
            return;
        }
        pos = (size_t)(nul - g.in) + 1;
        g.hello_done = true;
    }
    for (;;) {
        if (g.in_len - pos < 4) break;
        char hdr[5] = {0};
        memcpy(hdr, g.in + pos, 4);
        char* end;
        const unsigned long len = strtoul(hdr, &end, 16);
        if (*end || len >= sizeof g.in - 4) {
            drop_client();
            return;
        }
        if (g.in_len - pos - 4 < len) break;
        char msg[sizeof g.in];
        memcpy(msg, g.in + pos + 4, len);
        msg[len] = 0;
        pos += 4 + len;
        handle_command(msg);
        if (g.client < 0) return;
    }
    memmove(g.in, g.in + pos, g.in_len - pos);
    g.in_len -= pos;
}

static int on_client(int fd, int events, void* data)
{
    (void)data;
    if (fd != g.client) return 0;
    if (events & (ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP) && !(events & ALOOPER_EVENT_INPUT)) {
        drop_client();
        return 0;
    }
    const ssize_t n = recv(fd, g.in + g.in_len, sizeof g.in - g.in_len, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        drop_client();
        return 0;
    }
    if (n > 0) {
        g.in_len += (size_t)n;
        parse_input();
    }
    return g.client >= 0 ? 1 : 0;
}

static int on_listen(int fd, int events, void* data)
{
    (void)events;
    (void)data;
    const int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0) return 1;
    drop_client();
    g.client = c;
    g.hello_done = false;
    g.in_len = 0;
    ALooper_addFd(g.looper, c, 0, ALOOPER_EVENT_INPUT, on_client, NULL);
    logmsg("%s", "guest connected");
    return 1;
}

static void send_pedo(int type, unsigned value)
{
    for (size_t i = 0; i < NMAP; i++) {
        if (kMap[i].type != type || !g.enabled[i]) continue;
        char text[64];
        snprintf(text, sizeof text, "%s:%u", kMap[i].event, value);
        send_frame(text);
    }
}

/* A step is a swing of |a| below and then above its running mean, at
 * most ~3.5 a second */
static void pedometer_feed(const ASensorEvent* e)
{
    const double m = sqrt((double)e->data[0] * e->data[0] + (double)e->data[1] * e->data[1] +
                          (double)e->data[2] * e->data[2]);
    const double t = (double)e->timestamp / 1e9;
    if (g.pedo.mean == 0) g.pedo.mean = m;
    g.pedo.mean += (m - g.pedo.mean) * 0.02;
    g.pedo.smooth += (m - g.pedo.mean - g.pedo.smooth) * 0.3;
    if (g.pedo.smooth < -0.6) g.pedo.armed = true;
    if (!g.pedo.armed || g.pedo.smooth < 1.2 || t - g.pedo.last_step < 0.28) return;
    g.pedo.armed = false;
    g.pedo.last_step = t;
    g.pedo.steps++;
    send_pedo(ASENSOR_TYPE_STEP_COUNTER, g.pedo.steps);
    send_pedo(ASENSOR_TYPE_STEP_DETECTOR, g.pedo.steps);
    g.pedo.recent[g.pedo.recent_n++ % SMD_STEPS] = t;
    const double oldest = g.pedo.recent[g.pedo.recent_n % SMD_STEPS];
    if (g.pedo.recent_n >= SMD_STEPS && t - oldest <= SMD_WINDOW && !g.pedo.smd_fired) {
        g.pedo.smd_fired = true; /* one-shot: the guest re-enables it */
        send_pedo(ASENSOR_TYPE_SIGNIFICANT_MOTION, ++g.pedo.smd_seq);
    }
}

static void forward(const ASensorEvent* e)
{
    if (e->type == ASENSOR_TYPE_ACCELEROMETER && pedometer_on()) pedometer_feed(e);
    for (size_t i = 0; i < NMAP; i++) {
        if (kMap[i].type != e->type || !g.enabled[i]) continue;
        char text[128];
        if (kMap[i].values == 1) {
            float v = e->data[0];
            /* The goldfish proximity sensor's range is 1: 0 is near, 1
             * far. Phones report cm (most just 0 or their range). */
            if (e->type == ASENSOR_TYPE_PROXIMITY) v = v < 1.0f ? 0.0f : 1.0f;
            snprintf(text, sizeof text, "%s:%g", kMap[i].event, v);
        } else {
            snprintf(text, sizeof text, "%s:%g:%g:%g", kMap[i].event, e->data[0], e->data[1], e->data[2]);
        }
        send_frame(text);
        return;
    }
}

static int on_sensors(int fd, int events, void* data)
{
    (void)fd;
    (void)events;
    (void)data;
    ASensorEvent ev[16];
    ssize_t n;
    while ((n = ASensorEventQueue_getEvents(g.queue, ev, 16)) > 0)
        for (ssize_t i = 0; i < n; i++) forward(&ev[i]);
    return 1;
}

static int on_synthetic(int fd, int events, void* data)
{
    (void)events;
    (void)data;
    uint64_t ticks;
    if (read(fd, &ticks, sizeof ticks) != sizeof ticks) return 1;
    for (size_t i = 0; i < NMAP; i++) {
        if (!synthetic(i) || pedometer(i) || !g.enabled[i]) continue;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double t = (double)now.tv_sec + now.tv_nsec / 1e9;
        /* weather-scale drift over hours, and the sensor's few-Pa noise */
        const double hpa = g.synth_base + 0.8 * sin(t / 5400.0) + ((double)rand() / RAND_MAX - 0.5) * 0.04;
        char text[64];
        snprintf(text, sizeof text, "%s:%.4f", kMap[i].event, hpa);
        send_frame(text);
    }
    return 1;
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
    /* The guest's HAL runs as another guest uid, but the same host uid */
    chmod(path, 0666);
    return s;
}

/* SensorService checks app ops against the caller's package: the app's,
 * from where it installed us (/data/app/<...>/<package>-<suffix>/lib/<abi>/) */
static const char* own_package(char* buf, size_t size)
{
    char exe[512];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return NULL;
    exe[n] = 0;
    char* lib = strstr(exe, "/lib/");
    if (!lib) return NULL;
    *lib = 0;
    const char* dir = strrchr(exe, '/');
    if (!dir) return NULL;
    snprintf(buf, size, "%s", dir + 1);
    /* Package names have no '-'; the base64 suffix can */
    char* dash = strchr(buf, '-');
    if (dash) *dash = 0;
    return buf[0] ? buf : NULL;
}

int main(int argc, char** argv)
{
    const char* path = NULL;
    const char* package = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--socket")) path = argv[i + 1];
        else if (!strcmp(argv[i], "--package")) package = argv[i + 1];
    }
    char package_buf[256];
    if (!package) package = own_package(package_buf, sizeof package_buf);
    if (!package) package = "com.hexadecinull.vineos";
    if (!path) {
        fprintf(stderr, "usage: vine_sensors --socket PATH [--package NAME]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    g.looper = ALooper_prepare(0);
    g.manager = ASensorManager_getInstanceForPackage(package);
    if (!g.manager) {
        logmsg("%s", "no sensor manager");
        return 1;
    }
    bool has_pressure = false;
    for (size_t i = 0; i < NMAP; i++) {
        if (pedometer(i)) continue;
        g.sensor[i] = ASensorManager_getDefaultSensor(g.manager, kMap[i].type);
        if (g.sensor[i]) logmsg("has %s", kMap[i].set_name);
        if (g.sensor[i] && kMap[i].type == ASENSOR_TYPE_PRESSURE) has_pressure = true;
    }
    if (!has_pressure) {
        g.synth_timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (g.synth_timer >= 0) {
            g.synth_pressure = true;
            srand((unsigned)time(NULL));
            g.synth_base = 1008.0 + (double)(rand() % 1000) / 100.0;
            ALooper_addFd(g.looper, g.synth_timer, 0, ALOOPER_EVENT_INPUT, on_synthetic, NULL);
            logmsg("%s", "made-up pressure");
        }
    }
    g.queue = ASensorManager_createEventQueue(g.manager, g.looper, LOOPER_ID_SENSORS, on_sensors, NULL);
    if (!g.queue) {
        logmsg("%s", "cannot create an event queue");
        return 1;
    }
    const int s = listen_on(path);
    if (s < 0) {
        logmsg("cannot listen on %s", path);
        return 1;
    }
    ALooper_addFd(g.looper, s, 0, ALOOPER_EVENT_INPUT, on_listen, NULL);
    fprintf(stderr, "vine_sensors: serving mask %#x on %s for %s\n", mask(), path, package);
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
}
