/*
 * image_check.h — decide whether the image in one flash region may run (REQ-005, REQ-006).
 *
 * Plain C with no chip registers, so tools/check_images.py can run the same code on the host
 * against the images the build makes.
 */
#ifndef IMAGE_CHECK_H
#define IMAGE_CHECK_H

#include <stdint.h>

typedef enum {
    IMAGE_OK = 0,
    IMAGE_EMPTY,            /* first word erased: nothing was written here */
    IMAGE_BAD_MAGIC,        /* not "FSIR" */
    IMAGE_BAD_FORMAT,
    IMAGE_BAD_KIND,         /* e.g. a last-resort image in a main-firmware slot */
    IMAGE_BAD_TARGET,       /* built for another controller */
    IMAGE_BAD_LENGTH,       /* body too short for a vector table, or does not fit the region */
    IMAGE_BAD_START,        /* reset handler outside this region: linked for another slot */
    IMAGE_BAD_BODY_CHECK,   /* CRC-32 of the body differs: damaged */
    IMAGE_BAD_SIGNATURE_A,
    IMAGE_BAD_SIGNATURE_B
} image_result_t;

/* Checks everything except the signatures, cheapest first: proprietary string, format, kind,
 * target, length, start address, body check value. `image` points at the region's first
 * byte; `region_start` is the address the region has on the chip (the same number on the
 * target, a different one on the host). On IMAGE_OK, *signed_bytes = header + body. */
image_result_t image_check_contents(const uint8_t *image, uint32_t region_start,
                                    uint32_t region_bytes, uint8_t kind,
                                    uint32_t *signed_bytes);

/* Signature A with key A, then Signature B with key B, over the first signed_bytes. */
image_result_t image_check_signatures(const uint8_t *image, uint32_t signed_bytes);

/* Short reason for a log line, e.g. "signature A". */
const char *image_result_text(image_result_t result);

/* Little-endian field reads; also used for the log lines. */
uint32_t image_read_u32(const uint8_t *bytes);

#endif /* IMAGE_CHECK_H */
