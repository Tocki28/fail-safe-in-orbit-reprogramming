/*
 * bl_image_check.h — the bootloader's own checks of the image in one flash region before it runs
 * it (REQ-005 damage, REQ-006 origin). Same meanings as common/image_check.h, which the
 * firmware's update receiver runs; written apart from it (bl_image_check.c says what differs),
 * so a defect in one is not in both (bl_crc32.h says why: owner's decision 35, 15 Oct 2026).
 *
 * The result codes are still defined once, in common/image_check.h (image_result_t), and the
 * header layout in common/image_format.h (Interfaces Sec. 5.5, Figure 6): definitions only, no
 * code. Both programs must accept and refuse exactly the same images, in the same order of
 * checks: the order numbers the "Check result" codes of Interfaces Sec. 5.4.
 */
#ifndef BL_IMAGE_CHECK_H
#define BL_IMAGE_CHECK_H

#include <stdint.h>

#include "image_check.h"

/* Checks everything except the signatures, in this order: first word erased, proprietary
 * string, format, kind, target, length, start address, body check value. `image` points at the
 * region's first byte; `region_start` is the address the region has on the chip. On IMAGE_OK,
 * *signed_bytes = header + body. A region too small to hold the header is a fault. */
image_result_t bl_image_check_contents(const uint8_t *image, uint32_t region_start,
                                       uint32_t region_bytes, uint8_t kind,
                                       uint32_t *signed_bytes);

/* Signature A with key A, then Signature B with key B, over the first signed_bytes. */
image_result_t bl_image_check_signatures(const uint8_t *image, uint32_t signed_bytes);

/* Short reason for a log line, e.g. "signature A"; the same texts as image_result_text. */
const char *bl_image_result_text(image_result_t result);

/* Little-endian 32-bit field read; also used for the log lines. */
uint32_t bl_image_read_u32(const uint8_t *bytes);

#endif /* BL_IMAGE_CHECK_H */
