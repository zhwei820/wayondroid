/* vine_modem --socket PATH --seed TEXT [--sim-operator MCCMNC] [--sim-name NAME]
 *            [--net-operator MCCMNC] [--net-name NAME]
 *
 * The phone's SIM for a rootless guest. The guest runs the emulator's RIL
 * (libgoldfish-rild with libcuttlefish-ril-2.so), which libvine_ril_shim
 * connects to PATH, and talks AT commands over it; this answers them as an LTE modem holding
 * a SIM of the phone's carrier. Without --sim-operator the SIM is absent.
 *
 * Only what the phone tells any app goes in: the carrier codes and names.
 * IMSI, ICCID and IMEI are made up from --seed (the instance id), so they
 * stay the same across boots of one instance. The responses follow
 * cuttlefish's modem_simulator (android13-release), which that RIL is
 * written against. Data calls fail: the guest's network is its eth0. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "vine_modem.h"

/* What the RIL's +CREG / +CEREG parsing and the cuttlefish modem use */
#define AREA_CODE "2142"
#define CELL_ID "0000B804"
#define ACT_LTE 7
#define TECH_LTE 5 /* +CTEC: bit index of MDM_LTE */
/* Android's RegState NOT_REG_MT_SEARCHING_OP_EM, which the RIL passes on:
 * a phone without a SIM camps on a cell for emergency calls only */
#define REG_EMERGENCY 12

static void put(struct modem_out* out, const char* fmt, ...)
{
    if (out->len >= sizeof out->buf) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out->buf + out->len, sizeof out->buf - out->len, fmt, ap);
    va_end(ap);
    if (n > 0) out->len = out->len + (size_t)n < sizeof out->buf ? out->len + (size_t)n : sizeof out->buf - 1;
}

/* One response line, framed as a modem frames it */
static void line(struct modem_out* out, const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(out, "\r\n%s\r\n", buf);
}

static uint64_t next_random(uint64_t* s)
{
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

static void random_digits(uint64_t* s, char* dst, size_t n)
{
    for (size_t i = 0; i < n; i++) dst[i] = (char)('0' + next_random(s) % 10);
    dst[n] = 0;
}

/* The Luhn check digit for digits */
static char luhn(const char* digits)
{
    const size_t n = strlen(digits);
    int sum = 0;
    for (size_t i = 0; i < n; i++) {
        int d = digits[n - 1 - i] - '0';
        if (i % 2 == 0) {
            d *= 2;
            if (d > 9) d -= 9;
        }
        sum += d;
    }
    return (char)('0' + (10 - sum % 10) % 10);
}

static bool all_digits(const char* s, size_t min, size_t max)
{
    const size_t n = strlen(s);
    if (n < min || n > max) return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

/* Names go out inside "..." in AT responses: no quotes or control bytes */
static void copy_name(char* dst, size_t size, const char* src)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < size; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c != '"' && c != 0x7f) dst[j++] = (char)c;
    }
    dst[j] = 0;
}

/* Decodes one UTF-8 character; returns its length, 0 when invalid */
static size_t utf8_next(const unsigned char* s, unsigned* cp)
{
    if (s[0] < 0x80) {
        *cp = s[0];
        return 1;
    }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        *cp = ((s[0] & 0x1fu) << 6) | (s[1] & 0x3fu);
        return 2;
    }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *cp = ((s[0] & 0x0fu) << 12) | ((s[1] & 0x3fu) << 6) | (s[2] & 0x3fu);
        return 3;
    }
    return 0;
}

/* EF_SPN (TS 31.102 4.2.12): display condition, then 16 bytes of the name
 * in the GSM default alphabet, or UCS2 after 0x80 when it is not ASCII
 * letters, digits and spaces. Longer names are cut. */
static void encode_spn(const char* name, uint8_t spn[17])
{
    memset(spn, 0xff, 17);
    spn[0] = 0x01; /* display the registered PLMN as well */
    bool plain = true;
    for (const char* p = name; *p; p++) {
        const char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ')) plain = false;
    }
    if (plain) {
        for (size_t i = 0; name[i] && i < 16; i++) spn[1 + i] = (uint8_t)name[i];
        return;
    }
    spn[1] = 0x80;
    size_t at = 2;
    const unsigned char* s = (const unsigned char*)name;
    unsigned cp;
    size_t n;
    while (*s && at + 1 < 17 && (n = utf8_next(s, &cp)) != 0) {
        spn[at++] = (uint8_t)(cp >> 8);
        spn[at++] = (uint8_t)cp;
        s += n;
    }
}

static void hex(const uint8_t* bytes, size_t n, char* dst)
{
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        dst[2 * i] = digits[bytes[i] >> 4];
        dst[2 * i + 1] = digits[bytes[i] & 15];
    }
    dst[2 * n] = 0;
}

/* EF_ICCID: BCD, low nibble first */
static void encode_iccid(const char* iccid, char* dst)
{
    uint8_t b[10];
    for (size_t i = 0; i < 10; i++) {
        const unsigned lo = iccid[2 * i] ? (unsigned)(iccid[2 * i] - '0') : 0xf;
        const unsigned hi = iccid[2 * i] && iccid[2 * i + 1] ? (unsigned)(iccid[2 * i + 1] - '0') : 0xf;
        b[i] = (uint8_t)(hi << 4 | lo);
    }
    hex(b, 10, dst);
}

void modem_init(struct modem* m, const char* seed, const char* sim_operator, const char* sim_name,
                const char* net_operator, const char* net_name)
{
    memset(m, 0, sizeof *m);
    m->creg_mode = 2;
    m->cops_format = 0;
    uint64_t s = 0xcbf29ce484222325ull; /* FNV-1a */
    for (const char* p = seed ? seed : ""; *p; p++) s = (s ^ (unsigned char)*p) * 0x100000001b3ull;
    if (!s) s = 1;

    char body[16];
    random_digits(&s, body, 12);
    snprintf(m->imei, sizeof m->imei, "35%s", body);
    m->imei[14] = luhn(m->imei);
    m->imei[15] = 0;

    m->sim = sim_operator && all_digits(sim_operator, 5, 6);
    if (!m->sim) return;
    snprintf(m->sim_operator, sizeof m->sim_operator, "%s", sim_operator);
    copy_name(m->sim_name, sizeof m->sim_name, sim_name);
    if (net_operator && all_digits(net_operator, 5, 6)) {
        snprintf(m->net_operator, sizeof m->net_operator, "%s", net_operator);
        copy_name(m->net_name, sizeof m->net_name, net_name);
    } else {
        snprintf(m->net_operator, sizeof m->net_operator, "%s", m->sim_operator);
        snprintf(m->net_name, sizeof m->net_name, "%s", m->sim_name);
    }
    if (!m->net_name[0]) snprintf(m->net_name, sizeof m->net_name, "%s", m->net_operator);

    const size_t n = strlen(m->sim_operator);
    random_digits(&s, body, 15 - n);
    snprintf(m->imsi, sizeof m->imsi, "%s%s", m->sim_operator, body);
    random_digits(&s, body, 19 - 2 - n);
    snprintf(m->iccid, sizeof m->iccid, "89%s%s", m->sim_operator, body);
    m->iccid[19] = luhn(m->iccid);
    m->iccid[20] = 0;
}

/* +CRSM for the files a SIM's records load reads; the rest are not found.
 * GET RESPONSE answers are USIM FCP templates (the RIL turns them into
 * SIM ones) from cuttlefish's iccprofile_for_sim0.xml. */
static void sim_io(struct modem* m, struct modem_out* out, const char* args)
{
    int cmd, fid, p1, p2, p3;
    if (sscanf(args, "%d,%d,%d,%d,%d", &cmd, &fid, &p1, &p2, &p3) != 5) {
        line(out, "ERROR");
        return;
    }
    char data[80] = "";
    const char* fcp = NULL;
    switch (fid) {
    case 0x2fe2: /* EF_ICCID */
        fcp = "62178202412183022FE28A01058B032F06038002000A880110";
        encode_iccid(m->iccid, data);
        break;
    case 0x2f05: /* EF_PL */
        fcp = "62178202412183022F058A01058B032F060280020004880128";
        strcpy(data, "FFFFFFFF");
        break;
    case 0x6fad: /* EF_AD: the MNC's length */
        fcp = "62178202412183026FAD8A01058B036F060180020004880118";
        snprintf(data, sizeof data, "0000000%zu", strlen(m->sim_operator) - 3);
        break;
    case 0x6f46: { /* EF_SPN */
        if (!m->sim_name[0]) break;
        fcp = "62178202412183026F468A01058B036F060180020011880120";
        uint8_t spn[17];
        encode_spn(m->sim_name, spn);
        hex(spn, sizeof spn, data);
        break;
    }
    case 0x6f40: /* EF_MSISDN: one empty record */
        fcp = "621982054221001C0283026F408A01058B036F0605800200388800";
        memset(data, 'F', 56);
        data[56] = 0;
        break;
    }
    if (!fcp) {
        line(out, "+CRSM: 106,130");
    } else if (cmd == 192) {
        line(out, "+CRSM: 144,0,%s", fcp);
    } else if (cmd == 176 || (cmd == 178 && p1 == 1)) {
        line(out, "+CRSM: 144,0,%s", data);
    } else if (cmd == 178) {
        line(out, "+CRSM: 106,131"); /* record not found */
    } else {
        line(out, "+CRSM: 105,130"); /* updates: not allowed */
    }
    line(out, "OK");
}

/* Circuit-switched (+CREG) or packet (+CGREG, +CEREG); only the former
 * has emergency service without a SIM */
static void registration(struct modem* m, struct modem_out* out, const char* prefix, bool voice)
{
    if (m->sim && m->radio_on)
        line(out, "%s %d,1,\"%s\",\"%s\",%d", prefix, m->creg_mode, AREA_CODE, CELL_ID, ACT_LTE);
    else if (m->radio_on && voice)
        line(out, "%s %d,%d,\"%s\",\"%s\",%d", prefix, m->creg_mode, REG_EMERGENCY, AREA_CODE, CELL_ID, ACT_LTE);
    else
        line(out, "%s %d,0", prefix, m->creg_mode);
    line(out, "OK");
}

static bool starts(const char* s, const char* prefix) { return !strncmp(s, prefix, strlen(prefix)); }

/* One command, without "AT" (and its leading '+' kept) */
static void command(struct modem* m, struct modem_out* out, const char* c)
{
    if (!strcmp(c, "+CFUN?")) {
        line(out, "+CFUN: %d", m->radio_on);
        line(out, "OK");
    } else if (starts(c, "+CFUN=")) {
        const bool on = atoi(c + 6) == 1;
        line(out, "OK");
        if (on != m->radio_on) {
            m->radio_on = on;
            /* the RIL polls registration on this */
            line(out, "+CREG: %d", !on ? 0 : m->sim ? 1 : REG_EMERGENCY);
        }
    } else if (!strcmp(c, "+CPIN?")) {
        if (m->sim) {
            line(out, "+CPIN: READY");
            line(out, "OK");
        } else {
            line(out, "+CME ERROR: 10");
        }
    } else if (!strcmp(c, "+CIMI") || !strcmp(c, "+CICCID")) {
        if (m->sim) {
            line(out, "%s", !strcmp(c, "+CIMI") ? m->imsi : m->iccid);
            line(out, "OK");
        } else {
            line(out, "+CME ERROR: 10");
        }
    } else if (!strcmp(c, "+CGSN") || !strcmp(c, "+CGSN=1")) {
        line(out, "%s", m->imei);
        line(out, "OK");
    } else if (starts(c, "+CRSM=")) {
        if (m->sim) sim_io(m, out, c + 6);
        else line(out, "+CME ERROR: 10");
    } else if (starts(c, "+CREG=")) {
        m->creg_mode = atoi(c + 6);
        line(out, "OK");
    } else if (!strcmp(c, "+CREG?")) {
        registration(m, out, "+CREG:", true);
    } else if (!strcmp(c, "+CGREG?")) {
        registration(m, out, "+CGREG:", false);
    } else if (!strcmp(c, "+CEREG?")) {
        registration(m, out, "+CEREG:", false);
    } else if (!strcmp(c, "+COPS?")) {
        if (!m->sim || !m->radio_on) line(out, "+COPS: 0");
        else if (m->cops_format == 2) line(out, "+COPS: 0,2,\"%s\"", m->net_operator);
        else line(out, "+COPS: 0,%d,\"%s\"", m->cops_format, m->net_name);
        line(out, "OK");
    } else if (starts(c, "+COPS=3,")) {
        m->cops_format = atoi(c + 8);
        line(out, "OK");
    } else if (starts(c, "+COPS=?")) {
        line(out, "+CME ERROR: 3");
    } else if (starts(c, "+COPS=")) {
        line(out, "OK");
    } else if (!strcmp(c, "+CTEC?")) {
        line(out, "+CTEC: %d,23", TECH_LTE); /* preferred: LTE | WCDMA | GSM */
        line(out, "OK");
    } else if (!strcmp(c, "+CTEC=?")) {
        line(out, "+CTEC: 0,1,5");
        line(out, "OK");
    } else if (starts(c, "+CTEC=")) {
        line(out, "+CTEC: %d", TECH_LTE);
        line(out, "OK");
    } else if (!strcmp(c, "+CSQ")) {
        /* gsm, cdma, evdo unknown; LTE rssi 20, rsrp -90 dBm; the rest
         * unknown. The cell camped on is heard with a SIM or without. */
        if (m->radio_on)
            line(out, "+CSQ: 99,99,-1,-1,-1,-1,-1,20,90,%d,%d,%d,%d,%d,99,99,%d,%d,%d,%d,%d,%d", INT32_MAX, INT32_MAX,
                 INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX);
        else
            line(out, "+CSQ: 99,99,-1,-1,-1,-1,-1,99,%d,%d,%d,%d,%d,99,99,%d,%d,%d,%d,%d,%d", INT32_MAX, INT32_MAX,
                 INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX);
        line(out, "OK");
    } else if (starts(c, "+CLCK=")) {
        /* queries (mode 2) of the SIM and FDN locks: off */
        if (strstr(c, ",2")) line(out, "+CLCK: 0");
        line(out, "OK");
    } else if (starts(c, "D") || !strcmp(c, "A") || starts(c, "+CGACT=") || starts(c, "+CGDATA") ||
               starts(c, "+CGCONTRDP") || starts(c, "+CMGS=") || starts(c, "+CMGW=") || starts(c, "+CCHO=") ||
               starts(c, "+CGLA=") || starts(c, "+CSIM=") || starts(c, "+CUSD=")) {
        /* calls, data, SMS, logical channels: none */
        line(out, "ERROR");
    } else if (!strcmp(c, "+CLCC") || !strcmp(c, "+CGACT?") || !strcmp(c, "+CGDCONT?")) {
        line(out, "OK"); /* no calls or contexts */
    } else if (strchr(c, '?')) {
        line(out, "ERROR");
    } else {
        line(out, "OK"); /* settings: E0Q0V1, +CMEE=1, +CNMI=..., %CPHS=1, ... */
    }
}

void modem_handle(struct modem* m, const char* request, struct modem_out* out)
{
    out->len = 0;
    out->buf[0] = 0;
    while (*request == ' ') request++;
    if (strncasecmp(request, "AT", 2)) {
        if (*request) line(out, "ERROR");
        return;
    }
    const char* body = request + 2;
    /* "AT+COPS=3,0;+COPS?;..." runs each part and ends with one final
     * result; a dial string's trailing ';' is part of the dial */
    if (*body == 'D' || *body == 'd' || !strchr(body, ';')) {
        command(m, out, body);
        return;
    }
    char copy[512];
    snprintf(copy, sizeof copy, "%s", body);
    struct modem_out part;
    char* save = NULL;
    for (char* c = strtok_r(copy, ";", &save); c; c = strtok_r(NULL, ";", &save)) {
        part.len = 0;
        part.buf[0] = 0;
        command(m, &part, c);
        const char* end = part.buf + part.len;
        /* drop each part's final "OK" but stop at an error */
        if (part.len >= 6 && !strcmp(end - 6, "\r\nOK\r\n")) {
            part.buf[part.len - 6] = 0;
            put(out, "%s", part.buf);
        } else {
            put(out, "%s", part.buf);
            return;
        }
    }
    line(out, "OK");
}

#ifndef VINE_MODEM_NO_MAIN
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
    /* The guest's RIL runs as the radio uid, but the same host uid */
    chmod(path, 0666);
    return s;
}

static bool write_all(int fd, const char* p, size_t n)
{
    while (n) {
        const ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* One RIL at a time: a new connection replaces the old one (rild restarted) */
static void serve(struct modem* m, int listener)
{
    int client = -1, logged = 0;
    char in[1024];
    size_t have = 0;
    for (;;) {
        struct pollfd p[2] = {{.fd = listener, .events = POLLIN}, {.fd = client, .events = POLLIN}};
        if (poll(p, client >= 0 ? 2 : 1, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (p[0].revents & POLLIN) {
            const int c = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                if (client >= 0) close(client);
                client = c;
                have = 0;
                logged = 0;
                m->radio_on = false;
                fprintf(stderr, "vine_modem: RIL connected\n");
            }
            continue;
        }
        if (client < 0 || !(p[1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        const ssize_t r = read(client, in + have, sizeof in - 1 - have);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            close(client);
            client = -1;
            fprintf(stderr, "vine_modem: RIL disconnected\n");
            continue;
        }
        have += (size_t)r;
        in[have] = 0;
        /* Commands end in '\r'; an SMS PDU in ^Z, but SMS sends are refused
         * before the RIL writes one */
        char* start = in;
        char* end;
        while ((end = strpbrk(start, "\r\n\x1a")) != NULL) {
            *end = 0;
            if (*start) {
                /* The first commands after a connection show how far the RIL got */
                if (logged < 300) {
                    logged++;
                    fprintf(stderr, "< %s\n", start);
                }
                struct modem_out out;
                modem_handle(m, start, &out);
                if (out.len && !write_all(client, out.buf, out.len)) break;
            }
            start = end + 1;
        }
        have = strlen(start);
        memmove(in, start, have);
        if (have == sizeof in - 1) have = 0; /* no line this long */
    }
}

int main(int argc, char** argv)
{
    const char *path = NULL, *seed = "", *sim_operator = NULL, *sim_name = "", *net_operator = NULL, *net_name = "";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--socket")) path = argv[i + 1];
        else if (!strcmp(argv[i], "--seed")) seed = argv[i + 1];
        else if (!strcmp(argv[i], "--sim-operator")) sim_operator = argv[i + 1];
        else if (!strcmp(argv[i], "--sim-name")) sim_name = argv[i + 1];
        else if (!strcmp(argv[i], "--net-operator")) net_operator = argv[i + 1];
        else if (!strcmp(argv[i], "--net-name")) net_name = argv[i + 1];
    }
    if (!path) {
        fprintf(stderr, "usage: vine_modem --socket PATH --seed TEXT [--sim-operator MCCMNC] [--sim-name NAME] "
                        "[--net-operator MCCMNC] [--net-name NAME]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    static struct modem m;
    modem_init(&m, seed, sim_operator, sim_name, net_operator, net_name);
    const int s = listen_on(path);
    if (s < 0) {
        fprintf(stderr, "vine_modem: cannot listen on %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (m.sim)
        fprintf(stderr, "vine_modem: SIM %s \"%s\" on %s \"%s\", serving %s\n", m.sim_operator, m.sim_name,
                m.net_operator, m.net_name, path);
    else
        fprintf(stderr, "vine_modem: no SIM, serving %s\n", path);
    serve(&m, s);
    return 1;
}
#endif
