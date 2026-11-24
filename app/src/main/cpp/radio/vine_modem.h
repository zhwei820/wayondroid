#pragma once

#include <stdbool.h>
#include <stddef.h>

struct modem {
    bool sim;               /* a SIM is inserted */
    bool radio_on;          /* +CFUN */
    int creg_mode;          /* +CREG=<n>, in every registration answer */
    int cops_format;        /* +COPS=3,<format>: 0 long name, 1 short, 2 numeric */
    char sim_operator[7];   /* MCC MNC */
    char sim_name[64];      /* the SIM's service provider name, UTF-8 */
    char net_operator[7];
    char net_name[64];
    char imsi[24];
    char iccid[28];
    char imei[24];
};

struct modem_out {
    char buf[2048];
    size_t len;
};

/* No SIM unless sim_operator is 5 or 6 digits. The network defaults to the SIM's. */
void modem_init(struct modem* m, const char* seed, const char* sim_operator, const char* sim_name,
                const char* net_operator, const char* net_name);

/* The modem's response to one command line (no line terminator) */
void modem_handle(struct modem* m, const char* request, struct modem_out* out);
