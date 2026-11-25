/* What the phone's touches carry that the guest's Wayland path doesn't:
 * Waydroid's hwcomposer (wl_seat v5, no wl_touch.shape) writes every
 * touch-down and touch-move into its input pipe with ABS_MT_PRESSURE 50
 * and no contact size. vine_wl appends one record per such event it sends,
 * in sending order, to this ring in the hwcomposer's XDG_RUNTIME_DIR;
 * hwc_shim.c (in the composer process) takes the next record for each such
 * batch the hwcomposer writes. Records are published by bumping `written`
 * after the record is in place. */
#pragma once

#include <stdint.h>

#define TOUCH_EXTRAS_FILE "touch-extras"
#define TOUCH_EXTRAS_MAGIC 0x54584531u /* "TXE1" */
#define TOUCH_EXTRAS_SLOTS 256

struct touch_extra {
    uint8_t pressure; /* 0..255 */
    uint8_t major;    /* contact axes, mm, 0..15 */
    uint8_t minor;
    uint8_t pad;
};

struct touch_extras {
    uint32_t magic;
    uint32_t written; /* records ever published */
    struct touch_extra slot[TOUCH_EXTRAS_SLOTS];
};
