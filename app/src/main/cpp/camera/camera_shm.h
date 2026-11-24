/* The frames vine_camera shares with the guest's camera HAL (camera_shim.c):
 * a header, then VINE_CAMERA_SLOTS frames of width x height I420. The
 * writer fills the slot after `latest`, then publishes it there; with three
 * slots a reader has two frame times to sample the one it took. */
#ifndef VINE_CAMERA_SHM_H
#define VINE_CAMERA_SHM_H

#include <stdint.h>

#define VINE_CAMERA_MAGIC 0x56434d31u /* "VCM1" */
#define VINE_CAMERA_SLOTS 3

struct vine_camera_shm {
    uint32_t magic;
    uint32_t width, height;
    uint32_t orientation; /* the phone camera's sensor orientation, degrees */
    uint32_t slot_offset, slot_size;
    _Atomic uint32_t latest; /* the last complete slot */
    _Atomic uint32_t frames; /* complete frames so far; 0: none yet */
};

#endif
