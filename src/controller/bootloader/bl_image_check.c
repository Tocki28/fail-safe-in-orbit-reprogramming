/*
 * bl_image_check.c — the bootloader's own checks of an image before it runs it (REQ-005 damage,
 * REQ-006 origin; bl_image_check.h; bl_crc32.h says why the bootloader has its own). Field
 * layout: common/image_format.h (Interfaces Sec. 5.5, Figure 6).
 *
 * What is written apart from common/image_check.c, which the firmware's update receiver runs:
 *   - the header fields are decoded once into a local record (decode_header), then judged in the
 *     interface's order; the proprietary string is compared as one little-endian word;
 *   - the region size is checked before the first header byte is read (a region smaller than
 *     the header is a fault), and the length check is written so it cannot wrap below zero;
 *   - the start address is judged as an offset into the region, not as an absolute address;
 *   - the two signatures are checked by one bounded loop over a table of key and refusal;
 *   - the CRC-32 is bl_crc32.c (another algorithm form) and the 32-bit read another loop.
 * What cannot differ: the order of the checks and their results (Interfaces Sec. 5.4, "Check
 * result"), the header layout, the CRC-32 (Interfaces Sec. 4.0), the keys (keys.h), Ed25519
 * (Monocypher, third_party/monocypher; shared for now, not yet decided by the owner) and the log
 * texts (the bench scenarios read them: test/controller/Makefile).
 *
 * Switches over an enum in this file have no default label: the compiler checks every case
 * (-Wswitch-enum, -Werror) and only a corrupted value reaches the line after the switch. This
 * deviates on purpose from MISRA C:2012 Rule 16.4 (a default in every switch): the rule's
 * purpose, handling a value outside the cases, is met by the fault reaction after the switch
 * (common/fault.h; owner's decision of 13 Oct 2026).
 */
#include "bl_image_check.h"

#include <stdbool.h>

#include "bl_crc32.h"
#include "fault.h"
#include "image_format.h"
#include "keys.h"
#include "layout.h"
#include "monocypher-ed25519.h"

/* Erased flash reads as all ones [RM0440 Rev 9, Sec. 5.7.5, PROGERR, p. 218]. */
#define ERASED_WORD           0xFFFFFFFFU
#define BYTES_PER_WORD        4U
#define BITS_PER_BYTE         8U
/* Vector table: word 0 = initial stack pointer, word 1 = reset handler
 * [Arm DUI 0553A, Sec. 2.3.4, Figure 2-2, p. 2-24]. */
#define RESET_VECTOR_OFFSET   BYTES_PER_WORD
#define VECTOR_WORDS_NEEDED   2U
/* Shortest body that still holds the zero fill and those two words. */
#define MIN_BODY_BYTES        ((uint32_t)(IMAGE_CODE_OFFSET - IMAGE_HEADER_BYTES) + \
                               (VECTOR_WORDS_NEEDED * BYTES_PER_WORD))
/* Header and two signatures take this much of every region. */
#define FRAME_BYTES           ((uint32_t)(IMAGE_HEADER_BYTES + IMAGE_SIGNATURES_BYTES))
/* Signature A and Signature B (Interfaces Sec. 5.5, Figure 5). */
#define SIGNATURES            2U

/* The proprietary string "FSIR" as the little-endian word its four bytes form
 * (common/image_format.h; Interfaces Sec. 5.5, Figure 6). */
static const uint32_t MAGIC_WORD = (uint32_t)IMAGE_MAGIC_0 | ((uint32_t)IMAGE_MAGIC_1 << 8U) |
                                   ((uint32_t)IMAGE_MAGIC_2 << 16U) |
                                   ((uint32_t)IMAGE_MAGIC_3 << 24U);

/* The header fields the checks use, decoded once (offsets: common/image_format.h). */
typedef struct {
    uint32_t first_word;   /* the proprietary string's 4 bytes; all ones when erased */
    uint8_t format;
    uint8_t kind;
    uint32_t target;       /* 16-bit field */
    uint32_t body_bytes;
    uint32_t body_check;
} header_fields_t;

/**
 * @brief Returns the 32-bit little-endian value at bytes.
 *
 * Multi-byte fields are little-endian (Interfaces Sec. 4.0). Built from the most significant
 * byte down, one byte per pass.
 *
 * @param[in]     bytes  first of the 4 bytes
 * @return the value
 */
uint32_t bl_image_read_u32(const uint8_t *bytes)
{
    uint32_t value = 0U;

    for (uint32_t i = BYTES_PER_WORD; i > 0U; i--) {
        value = (value << BITS_PER_BYTE) | (uint32_t)bytes[i - 1U];
    }
    return value;
}

/**
 * @brief Decodes the header fields the checks use.
 *
 * Reads only bytes inside the 64-byte header (offsets 0 to 19, common/image_format.h); the
 * caller has checked that the region holds the header.
 *
 * @param[in]     image   first byte of the region
 * @param[out]    fields  the decoded fields
 */
static void decode_header(const uint8_t *image, header_fields_t *fields)
{
    fields->first_word = bl_image_read_u32(&image[IMAGE_OFFSET_MAGIC]);
    fields->format = image[IMAGE_OFFSET_FORMAT];
    fields->kind = image[IMAGE_OFFSET_KIND];
    fields->target = (uint32_t)image[IMAGE_OFFSET_TARGET] |
                     ((uint32_t)image[IMAGE_OFFSET_TARGET + 1U] << BITS_PER_BYTE);
    fields->body_bytes = bl_image_read_u32(&image[IMAGE_OFFSET_LENGTH]);
    fields->body_check = bl_image_read_u32(&image[IMAGE_OFFSET_BODY_CHECK]);
}

/**
 * @brief Checks the header and the body of an image, not the signatures.
 *
 * Returns the first check that fails, in the order of Interfaces Sec. 5.4 ("Check result"), or
 * IMAGE_OK. A region smaller than the header is a fault: every caller passes a region of
 * layout.h (main.c, REGION_A/B/LAST_RESORT), so such a size is a corrupted value, and reading the
 * header would read past the region (code rules B1, B4). The length is checked before any byte
 * of the body is read, and is compared with the room left, so no subtraction goes below zero.
 * The start address is the reset handler's offset into the region: an address below the region
 * wraps to an offset far above any region and is refused with the rest. Why the start check: an
 * image linked for the other slot passes every other check (the header names no slot) but would
 * jump into the other slot's code; its reset handler gives it away.
 *
 * @param[in]     image         first byte of the region
 * @param[in]     region_start  address the region has on the chip
 * @param[in]     region_bytes  size of the region in bytes, at least the header
 * @param[in]     kind          the only image kind this region may run
 * @param[out]    signed_bytes  length of header plus body; set only on IMAGE_OK
 * @return IMAGE_OK, or the first check that failed
 * @note Does not return when region_bytes is smaller than the header.
 */
image_result_t bl_image_check_contents(const uint8_t *image, uint32_t region_start,
                                       uint32_t region_bytes, uint8_t kind,
                                       uint32_t *signed_bytes)
{
    header_fields_t fields;

    if (region_bytes < (uint32_t)IMAGE_HEADER_BYTES) {
        /* only a corrupted value reaches here: see above */
        fault_impossible_value(FAULT_AT_BL_IMAGE_CHECK_CONTENTS, "bl_image_check_contents");
    }
    decode_header(image, &fields);
    if (fields.first_word == ERASED_WORD) {
        return IMAGE_EMPTY;
    }
    if (fields.first_word != MAGIC_WORD) {
        return IMAGE_BAD_MAGIC;
    }
    if (fields.format != IMAGE_FORMAT_VERSION) {
        return IMAGE_BAD_FORMAT;
    }
    if (fields.kind != kind) {
        return IMAGE_BAD_KIND;
    }
    if (fields.target != IMAGE_TARGET_CONTROLLER) {
        return IMAGE_BAD_TARGET;
    }
    if ((fields.body_bytes < MIN_BODY_BYTES) || (fields.body_bytes > region_bytes) ||
        ((region_bytes - fields.body_bytes) < FRAME_BYTES)) {
        return IMAGE_BAD_LENGTH;
    }
    uint32_t entry = bl_image_read_u32(&image[IMAGE_CODE_OFFSET + RESET_VECTOR_OFFSET]);
    uint32_t entry_offset = entry - region_start;   /* unsigned: wraps when below the region */
    if ((entry_offset < (uint32_t)IMAGE_CODE_OFFSET) ||
        (entry_offset >= ((uint32_t)IMAGE_HEADER_BYTES + fields.body_bytes))) {
        return IMAGE_BAD_START;
    }
    if (bl_crc32_compute(&image[IMAGE_HEADER_BYTES], fields.body_bytes) != fields.body_check) {
        return IMAGE_BAD_BODY_CHECK;
    }
    *signed_bytes = (uint32_t)IMAGE_HEADER_BYTES + fields.body_bytes;
    return IMAGE_OK;
}

/**
 * @brief Checks Signature A with key A, then Signature B with key B, over the first signed_bytes.
 *
 * The signatures follow the body directly (Interfaces Sec. 5.5, Figure 5). signed_bytes is the
 * value bl_image_check_contents set with IMAGE_OK, which keeps the header, the body and both
 * signatures inside the region. crypto_ed25519_check returns 0 only for a valid signature
 * (third_party/monocypher/SOURCE). The loop runs at most SIGNATURES times and stops at the first
 * signature that fails.
 *
 * @param[in]     image         first byte of the region
 * @param[in]     signed_bytes  length of header plus body
 * @return IMAGE_OK, or IMAGE_BAD_SIGNATURE_A or IMAGE_BAD_SIGNATURE_B for the first signature that
 *         fails
 */
image_result_t bl_image_check_signatures(const uint8_t *image, uint32_t signed_bytes)
{
    const uint8_t *const keys[SIGNATURES] = {PUBLIC_KEY_A, PUBLIC_KEY_B};
    const image_result_t refusals[SIGNATURES] = {IMAGE_BAD_SIGNATURE_A, IMAGE_BAD_SIGNATURE_B};

    for (uint32_t s = 0U; s < SIGNATURES; s++) {
        const uint8_t *signature = &image[signed_bytes + (s * (uint32_t)IMAGE_SIGNATURE_BYTES)];
        if (crypto_ed25519_check(signature, keys[s], image, signed_bytes) != 0) {
            return refusals[s];
        }
    }
    return IMAGE_OK;
}

/**
 * @brief Returns the short text for a check result, for the log.
 *
 * The same texts as common/image_check.c, word for word: the bench scenarios look for them in
 * the "BL:" lines (test/controller/Makefile). Every image_result_t value has its own case that
 * returns, so -Wswitch-enum (common/build.mk) stops the build when a value is added and this
 * table is not. A value outside the enum is a corrupted variable and gets the fault reaction with
 * this function's own fault-location code (see the file comment; common/fault_codes.h).
 *
 * @param[in]     result  result of a check
 * @return text for the result
 * @note Does not return for a value outside image_result_t.
 */
const char *bl_image_result_text(image_result_t result)
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
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_BL_IMAGE_RESULT_TEXT, "bl_image_result_text");
}
