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
#include "flash.h"
#include "layout.h"

/* The check value covers every byte before it. */
#define CHECKED_BYTES  ((uint32_t)offsetof(boot_state_t, check))
/* The record is programmed as 64-bit flash words, two 32-bit words each. */
#define RECORD_WORDS   (sizeof(boot_state_t) / sizeof(uint32_t))
#define RECORD_PAIRS   (sizeof(boot_state_t) / sizeof(uint64_t))
/* Returned by newest_copy() when neither copy is valid. */
#define NO_VALID_COPY  2U

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
 * @param[in]     state  record to check
 * @return true if both slots and the count are allowed, false otherwise
 */
static bool fields_valid(const boot_state_t *state)
{
    bool confirmed_ok = (state->confirmed == BOOT_SLOT_A) || (state->confirmed == BOOT_SLOT_B);
    bool trial_ok = (state->trial == BOOT_SLOT_NONE) ||
                    (state->trial == boot_state_other_slot(state->confirmed));
    bool count_ok = (state->failed_starts <= BOOT_STATE_MAX_STARTS);
    return confirmed_ok && trial_ok && count_ok;
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
    flash_ecc_clear();
    memcpy(out, (const void *)(uintptr_t)COPY_START[copy], sizeof(*out));
    if (flash_ecc_error_seen()) {
        return false;
    }
    return (out->check == record_check(out)) && fields_valid(out);
}

/**
 * @brief The copy to trust: the valid one with the higher sequence number.
 *
 * Fills *out with it.
 *
 * @param[out]    out  record of the newest valid copy
 * @return number of that copy, 0 or 1; NO_VALID_COPY if neither is valid
 */
static uint32_t newest_copy(boot_state_t *out)
{
    boot_state_t copy0;
    boot_state_t copy1;
    bool ok0 = read_copy(0U, &copy0);
    bool ok1 = read_copy(1U, &copy1);

    if (ok0 && (!ok1 || (copy0.sequence >= copy1.sequence))) {
        *out = copy0;
        return 0U;
    }
    if (ok1) {
        *out = copy1;
        return 1U;
    }
    return NO_VALID_COPY;
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
 * @brief Returns slot B for slot A, slot A for slot B, and BOOT_SLOT_NONE for any other value.
 *
 * Before, every value that was not A answered A, so a damaged or empty code named a real slot
 * (the same lumping as bootloader/main.c, slot_region). An unknown code now has no other slot.
 * Caller: fields_valid, which also demands that confirmed is A or B, so an unknown code still
 * makes the record invalid.
 *
 * @param[in]     slot  BOOT_SLOT_A or BOOT_SLOT_B
 * @return the other slot code; BOOT_SLOT_NONE if slot is neither A nor B
 */
uint8_t boot_state_other_slot(uint8_t slot)
{
    if (slot == BOOT_SLOT_A) {
        return BOOT_SLOT_B;
    }
    if (slot == BOOT_SLOT_B) {
        return BOOT_SLOT_A;
    }
    return BOOT_SLOT_NONE;
}
