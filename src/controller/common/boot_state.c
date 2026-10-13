/*
 * boot_state.c — two-copy storage of the boot state (see boot_state.h).
 *
 * Wear: each write erases one page. A page lasts 10 000 erases [DS12589 Rev 6, Table 47,
 * p. 107] and writes alternate between the two copies, so the state survives about 20 000
 * writes. A write happens at every trial start, at every give-up of a trial, at every
 * confirmation and every SWITCH, and when the bootloader saves the default state because no
 * valid copy was found; a normal start with no trial writes nothing.
 * The same limit means the 32-bit sequence number never wraps.
 */
#include "boot_state.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"
#include "fault.h"
#include "flash.h"
#include "layout.h"

/* The check value covers every byte before it. */
#define CHECKED_BYTES  ((uint32_t)offsetof(boot_state_t, check))
/* The record is programmed as 64-bit flash words, two 32-bit words each. */
#define RECORD_WORDS   (sizeof(boot_state_t) / sizeof(uint32_t))
#define RECORD_PAIRS   (sizeof(boot_state_t) / sizeof(uint64_t))
/* Returned by pick_newest() when neither copy is valid. */
#define NO_VALID_COPY  2U
/* Two stored copies (layout.h, BOOT_STATE_COPY_0 and _1). */
#define COPIES         2U
/* An erased flash byte reads 0xFF [RM0440 Rev 9, Sec. 5.3.7, p. 197]. Erased flash raises no
 * two-bit ECC error when read [RM0440 Rev 9, Sec. 5.3.2, p. 190, Note: for virgin data two-error
 * detection is not supported], so a record never written reads as 16 bytes 0xFF, no ECC error. */
#define ERASED_BYTE    0xFFU

_Static_assert(sizeof(boot_state_t) == 16U, "boot state must be two 64-bit flash words");

static const uint32_t COPY_START[2] = {BOOT_STATE_COPY_0, BOOT_STATE_COPY_1};

/**
 * @brief Returns the check value of a record: the CRC-32 of the bytes before the check field.
 *
 * @param[in]     state  record to check
 * @return CRC-32 of the record
 */
static uint32_t record_check(const boot_state_t *state)
{
    return crc32_compute((const uint8_t *)state, CHECKED_BYTES);
}

/**
 * @brief Returns true if the slots and the start count of a record have allowed values.
 *
 * A record with a good check value must still name a real slot, the trial slot can only be the
 * slot that is not confirmed, and the start count cannot be above BOOT_STATE_MAX_STARTS: the
 * bootloader gives the trial up before it would store a higher count (bootloader/main.c,
 * count_trial_start), so a higher stored count is damage.
 *
 * The confirmed slot is checked first, and boot_state_other_slot() is asked only about a
 * confirmed slot that is A or B. Why: the record may come from flash and is being checked here; a
 * value read from flash that fails its check is damage, judged by this check and by decision D,
 * never the fault reaction (owner's fault decision of 13 Oct 2026, point 4). Before, a damaged
 * confirmed code went into boot_state_other_slot(), which then answered BOOT_SLOT_NONE; that
 * function now treats any code but A or B as a corrupted value.
 *
 * @param[in]     state  record to check
 * @return true if both slots and the count are allowed, false otherwise
 */
static bool fields_valid(const boot_state_t *state)
{
    if ((state->confirmed != BOOT_SLOT_A) && (state->confirmed != BOOT_SLOT_B)) {
        return false;
    }
    bool trial_ok = (state->trial == BOOT_SLOT_NONE) ||
                    (state->trial == boot_state_other_slot(state->confirmed));
    bool count_ok = (state->failed_starts <= BOOT_STATE_MAX_STARTS);
    return trial_ok && count_ok;
}

/**
 * @brief Returns true if a record passes its checks: the CRC-32 and the allowed fields (REQ-136).
 *
 * @param[in]     record  record copied to RAM
 * @return true if the check value matches and the fields are allowed, false otherwise
 */
static bool record_valid(const boot_state_t *record)
{
    return (record->check == record_check(record)) && fields_valid(record);
}

/**
 * @brief Returns true if every byte of a record is the erased value 0xFF: the copy was never
 * written since its last erase (decision D: "blank").
 *
 * @param[in]     record  record copied to RAM
 * @return true if all 16 bytes are 0xFF, false otherwise
 */
static bool record_blank(const boot_state_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;

    for (uint32_t i = 0U; i < (uint32_t)sizeof(*record); i++) {
        if (bytes[i] != ERASED_BYTE) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Copies one stored record to RAM, just before use (REQ-136), and reports a flash read
 * error.
 *
 * The bytes are copied whatever they hold; the checks come after (record_valid, record_blank).
 * Read through flash_at() so the host tests can serve a RAM copy of the flash.
 *
 * @param[in]     copy  number of the stored copy, 0 or 1
 * @param[out]    out   record read from flash
 * @return true if the read saw no two-bit ECC error, false if it did (a power cut during
 *         programming can leave such a word [RM0440 Rev 9, Sec. 5.3.2, p. 190])
 */
static bool read_raw(uint32_t copy, boot_state_t *out)
{
    flash_ecc_clear();
    memcpy(out, flash_at(COPY_START[copy]), sizeof(*out));
    return !flash_ecc_error_seen();
}

/**
 * @brief Copy one stored record to RAM and check it there, just before use (REQ-136).
 *
 * False if the copy is blank, damaged, or was cut short by a power loss (two-bit flash error).
 *
 * @param[in]     copy  number of the stored copy, 0 or 1
 * @param[out]    out   record read from flash
 * @return true if the copy is valid, false otherwise
 */
static bool read_copy(uint32_t copy, boot_state_t *out)
{
    return read_raw(copy, out) && record_valid(out);
}

/**
 * @brief The copy to trust: the valid one with the higher sequence number.
 *
 * @param[in]     copies  the two records
 * @param[in]     valid   valid[i] is true if copy i passed its checks
 * @return number of that copy, 0 or 1; NO_VALID_COPY if neither is valid
 */
static uint32_t pick_newest(const boot_state_t copies[COPIES], const bool valid[COPIES])
{
    if (valid[0] && (!valid[1] || (copies[0].sequence >= copies[1].sequence))) {
        return 0U;
    }
    if (valid[1]) {
        return 1U;
    }
    return NO_VALID_COPY;
}

/**
 * @brief The copy to trust, read from flash: the valid one with the higher sequence number.
 *
 * Fills *out with it when there is one.
 *
 * @param[out]    out  record of the newest valid copy; not written if neither is valid
 * @return number of that copy, 0 or 1; NO_VALID_COPY if neither is valid
 */
static uint32_t newest_copy(boot_state_t *out)
{
    boot_state_t copies[COPIES];
    bool valid[COPIES];

    for (uint32_t c = 0U; c < COPIES; c++) {
        valid[c] = read_copy(c, &copies[c]);
    }
    uint32_t newest = pick_newest(copies, valid);
    if (newest != NO_VALID_COPY) {
        *out = copies[newest];
    }
    return newest;
}

/**
 * @brief Decides what two records read from flash hold: a valid state, nothing yet, or damage
 * (decision D, owner, 13 Oct 2026).
 *
 * VALID: at least one copy passed its checks; *state is the newest such copy (the rule of
 * pick_newest). BLANK: both records read without a flash error and are all 0xFF; the bootloader
 * then uses the default state (confirmed A, no trial), as before. DAMAGED: neither copy is valid
 * and at least one is not blank, or a read saw a flash ECC error; the bootloader then starts only
 * the last-resort firmware. Why the difference: an erased record is what a fresh chip and a
 * restore leave (Architecture Sec. 5.1: the restore clears the kept data), so the default is
 * right there; a written record that fails its checks says the stored state is lost, and taking
 * the default could start a slot the operator had given up (REQ-019).
 *
 * @param[in]     copies    the two records as read
 * @param[in]     readable  readable[i] is false if reading copy i saw a flash ECC error
 * @param[out]    state     the newest valid copy; written only for BOOT_STATE_VALID
 * @return BOOT_STATE_VALID, BOOT_STATE_BLANK or BOOT_STATE_DAMAGED
 */
boot_state_load_t boot_state_classify(const boot_state_t copies[2], const bool readable[2],
                                      boot_state_t *state)
{
    bool valid[COPIES];

    for (uint32_t c = 0U; c < COPIES; c++) {
        valid[c] = readable[c] && record_valid(&copies[c]);
    }
    uint32_t newest = pick_newest(copies, valid);
    if (newest != NO_VALID_COPY) {
        *state = copies[newest];
        return BOOT_STATE_VALID;
    }
    if (readable[0] && readable[1] && record_blank(&copies[0]) && record_blank(&copies[1])) {
        return BOOT_STATE_BLANK;
    }
    return BOOT_STATE_DAMAGED;
}

/**
 * @brief Reads both stored copies and classifies them (boot_state_classify; decision D).
 *
 * @param[out]    state  the newest valid copy; written only for BOOT_STATE_VALID
 * @return BOOT_STATE_VALID, BOOT_STATE_BLANK or BOOT_STATE_DAMAGED
 */
boot_state_load_t boot_state_load(boot_state_t *state)
{
    boot_state_t copies[COPIES];
    bool readable[COPIES];

    for (uint32_t c = 0U; c < COPIES; c++) {
        readable[c] = read_raw(c, &copies[c]);
    }
    return boot_state_classify(copies, readable, state);
}

/**
 * @brief Reads the boot state into *state from the newest valid copy.
 *
 * @param[out]    state  boot state read from flash
 * @return true if a valid copy was read, false if none is valid
 */
bool boot_state_read(boot_state_t *state)
{
    return newest_copy(state) != NO_VALID_COPY;
}

/**
 * @brief Writes the boot state to the copy that is not current, then reads it back and compares.
 *
 * Sets the sequence number and the check value in *state.
 *
 * @param[in,out] state  boot state to write; sequence, unused, reserved and check are set
 * @return true if the state was written and read back equal; false if the state is not valid
 * (slots or start count not allowed), or the erase, the programming or the read-back failed
 */
bool boot_state_write(boot_state_t *state)
{
    boot_state_t current;
    uint32_t newest;
    uint32_t target;
    uint32_t words[RECORD_WORDS];
    boot_state_t back;

    /* Checked before any flash is read, so a record that is not allowed touches nothing. */
    if (!fields_valid(state)) {
        return false;
    }
    newest = newest_copy(&current);
    /* Overwrite the copy that is NOT current, so the current one survives a power cut. */
    target = (newest == 0U) ? 1U : 0U;
    state->sequence = (newest == NO_VALID_COPY) ? 1U : (current.sequence + 1U);
    state->unused = 0U;
    state->reserved = 0U;
    state->check = record_check(state);
    memcpy(words, state, sizeof(words));

    /* The record sits at the start of the copy, so erasing the copy's first page is enough. */
    bool ok = flash_erase_page(COPY_START[target]) &&
              flash_program(COPY_START[target], words, RECORD_PAIRS);
    /* Read back through the same checks a later start will use. */
    return ok && read_copy(target, &back) && (memcmp(&back, state, sizeof(back)) == 0);
}

/**
 * @brief Returns slot B for slot A and slot A for slot B; any other code is a fault.
 *
 * The only caller, fields_valid, asks only about a confirmed slot it has already found to be A or
 * B. So any other code here is a corrupted value in memory, and it gets the fault reaction
 * (common/fault.h; owner's fault decision of 13 Oct 2026). Before, it answered BOOT_SLOT_NONE,
 * and before that slot A: a normal-looking answer for a value that cannot exist.
 *
 * @param[in]     slot  BOOT_SLOT_A or BOOT_SLOT_B
 * @return the other slot code
 * @note Does not return for any code but BOOT_SLOT_A or BOOT_SLOT_B.
 */
uint8_t boot_state_other_slot(uint8_t slot)
{
    if (slot == BOOT_SLOT_A) {
        return BOOT_SLOT_B;
    }
    if (slot == BOOT_SLOT_B) {
        return BOOT_SLOT_A;
    }
    fault_impossible_value("boot_state_other_slot");   /* only a corrupted value: see above */
}
