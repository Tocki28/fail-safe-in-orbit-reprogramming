/*
 * image_check.c — the checks an image passes before the bootloader runs it (REQ-005 damage,
 * REQ-006 origin). Field layout: image_format.h (Interfaces Sec. 5.5, Figure 6).
 */
#include "image_check.h"

#include <stdbool.h>

#include "crc32.h"
#include "image_format.h"
#include "keys.h"
#include "layout.h"
#include "monocypher-ed25519.h"

/* Erased flash reads as all ones [RM0440 Rev 9, Sec. 5.7.5, PROGERR, p. 218]. */
#define ERASED_WORD           0xFFFFFFFFU
#define BYTES_PER_WORD        4U
/* Vector table: word 0 = initial stack pointer, word 1 = reset handler
 * [Arm DUI 0553A, Sec. 2.3.4, Figure 2-2, p. 2-24]. */
#define RESET_VECTOR_OFFSET   BYTES_PER_WORD
#define VECTOR_WORDS_NEEDED   2U
/* Shortest body that still holds the zero fill and those two words. */
#define MIN_BODY_BYTES        ((uint32_t)(IMAGE_CODE_OFFSET - IMAGE_HEADER_BYTES) + \
                               (VECTOR_WORDS_NEEDED * BYTES_PER_WORD))
/* Header and two signatures take this much of every region. */
#define FRAME_BYTES           ((uint32_t)(IMAGE_HEADER_BYTES + IMAGE_SIGNATURES_BYTES))

/**
 * @brief Returns the 32-bit little-endian value at bytes.
 *
 * @param[in]     bytes  first of the 4 bytes
 * @return the value
 */
uint32_t image_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/**
 * @brief Returns the 16-bit little-endian value at bytes.
 *
 * @param[in]     bytes  first of the 2 bytes
 * @return the value
 */
static uint32_t read_u16(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
}

/**
 * @brief Returns true if the first four bytes of the image are the proprietary string.
 *
 * @param[in]     image  first byte of the image
 * @return true if they match, false otherwise
 */
static bool magic_ok(const uint8_t *image)
{
    return (image[IMAGE_OFFSET_MAGIC] == IMAGE_MAGIC_0) &&
           (image[IMAGE_OFFSET_MAGIC + 1U] == IMAGE_MAGIC_1) &&
           (image[IMAGE_OFFSET_MAGIC + 2U] == IMAGE_MAGIC_2) &&
           (image[IMAGE_OFFSET_MAGIC + 3U] == IMAGE_MAGIC_3);
}

/**
 * @brief Checks the header and the body of an image, not the signatures.
 *
 * Returns the first check that fails, or IMAGE_OK. Details: image_check.h.
 *
 * @param[in]     image         first byte of the region
 * @param[in]     region_start  address the region has on the chip
 * @param[in]     region_bytes  size of the region in bytes
 * @param[in]     kind          the only image kind this region may run
 * @param[out]    signed_bytes  length of header plus body; set only on IMAGE_OK
 * @return IMAGE_OK, or the first check that failed
 */
image_result_t image_check_contents(const uint8_t *image, uint32_t region_start,
                                    uint32_t region_bytes, uint8_t kind,
                                    uint32_t *signed_bytes)
{
    if (image_read_u32(image) == ERASED_WORD) {
        return IMAGE_EMPTY;
    }
    if (!magic_ok(image)) {
        return IMAGE_BAD_MAGIC;
    }
    if (image[IMAGE_OFFSET_FORMAT] != IMAGE_FORMAT_VERSION) {
        return IMAGE_BAD_FORMAT;
    }
    if (image[IMAGE_OFFSET_KIND] != kind) {
        return IMAGE_BAD_KIND;
    }
    if (read_u16(&image[IMAGE_OFFSET_TARGET]) != IMAGE_TARGET_CONTROLLER) {
        return IMAGE_BAD_TARGET;
    }

    /* Length is checked before any byte of the body is read, so a damaged length cannot make
     * the checks below read past the region. */
    uint32_t body = image_read_u32(&image[IMAGE_OFFSET_LENGTH]);
    if ((body < MIN_BODY_BYTES) || (body > (region_bytes - FRAME_BYTES))) {
        return IMAGE_BAD_LENGTH;
    }

    /* An image linked for the other slot passes every other check (the header names no
     * slot) but would jump into the other slot's code. Its reset handler gives it away. */
    uint32_t code_start = region_start + (uint32_t)IMAGE_CODE_OFFSET;
    uint32_t code_end = region_start + (uint32_t)IMAGE_HEADER_BYTES + body;
    uint32_t reset_handler = image_read_u32(&image[IMAGE_CODE_OFFSET + RESET_VECTOR_OFFSET]);
    if ((reset_handler < code_start) || (reset_handler >= code_end)) {
        return IMAGE_BAD_START;
    }

    uint32_t stored_check = image_read_u32(&image[IMAGE_OFFSET_BODY_CHECK]);
    if (crc32_compute(&image[IMAGE_HEADER_BYTES], body) != stored_check) {
        return IMAGE_BAD_BODY_CHECK;
    }

    *signed_bytes = (uint32_t)IMAGE_HEADER_BYTES + body;
    return IMAGE_OK;
}

/**
 * @brief Checks signature A with key A, then signature B with key B, over the first signed_bytes.
 *
 * @param[in]     image         first byte of the region
 * @param[in]     signed_bytes  length of header plus body
 * @return IMAGE_OK, or IMAGE_BAD_SIGNATURE_A or IMAGE_BAD_SIGNATURE_B for the first signature that
 * fails
 */
image_result_t image_check_signatures(const uint8_t *image, uint32_t signed_bytes)
{
    /* The signatures follow the body directly (Interfaces Sec. 5.5, Figure 5). */
    const uint8_t *signature_a = &image[signed_bytes];
    const uint8_t *signature_b = &signature_a[IMAGE_SIGNATURE_BYTES];

    /* crypto_ed25519_check returns 0 only for a valid signature (third_party/monocypher/SOURCE). */
    if (crypto_ed25519_check(signature_a, PUBLIC_KEY_A, image, signed_bytes) != 0) {
        return IMAGE_BAD_SIGNATURE_A;
    }
    if (crypto_ed25519_check(signature_b, PUBLIC_KEY_B, image, signed_bytes) != 0) {
        return IMAGE_BAD_SIGNATURE_B;
    }
    return IMAGE_OK;
}

/**
 * @brief Returns the short text for a check result, for the log.
 *
 * Every image_result_t value has its own case, so -Wswitch-enum (common/build.mk) stops the
 * build when a value is added and this table is not. The default is only for a value outside the
 * enum (a corrupted variable; MISRA C:2012 Rule 16.4 asks for a default in every switch): it
 * logs "corrupted result value", so the log says what happened. The image is still refused:
 * image_passes() starts an image only on IMAGE_OK.
 *
 * @param[in]     result  result of a check
 * @return text for the result; "corrupted result value" for a value outside image_result_t
 */
const char *image_result_text(image_result_t result)
{
    switch (result) {
    case IMAGE_OK:              return "passed";
    case IMAGE_EMPTY:           return "empty";
    case IMAGE_BAD_MAGIC:       return "proprietary string";
    case IMAGE_BAD_FORMAT:      return "format";
    case IMAGE_BAD_KIND:        return "kind";
    case IMAGE_BAD_TARGET:      return "target";
    case IMAGE_BAD_LENGTH:      return "length";
    case IMAGE_BAD_START:       return "start address outside the region";
    case IMAGE_BAD_BODY_CHECK:  return "body check value";
    case IMAGE_BAD_SIGNATURE_A: return "signature A";
    case IMAGE_BAD_SIGNATURE_B: return "signature B";
    default:                    return "corrupted result value";   /* outside the enum only */
    }
}
