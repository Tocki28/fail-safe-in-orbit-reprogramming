/*
 * bl_boot_state.c — the bootloader's own two-copy storage of the boot state (bl_boot_state.h;
 * bl_crc32.h says why the bootloader has its own).
 *
 * What is written apart from common/boot_state.c, which the firmware runs:
 *   - the record is encoded and decoded field by field at fixed byte offsets (encode, decode),
 *     where common/boot_state.c copies the struct's memory as it is. The offsets are checked
 *     against boot_state_t at build time, so the bytes in flash are the same, but a change in the
 *     struct's layout now breaks the build here instead of changing the record unnoticed;
 *   - the check value is the CRC-32 of the 12 encoded bytes (bl_crc32.c, another algorithm form);
 *   - "blank" is tested field by field against the erased values, the newest valid copy is found
 *     by one loop with a strict "higher than", and the copy a write goes to is taken from a table
 *     of "the other copy" in the same loop's result;
 *   - the slot fields are judged without asking bl_boot_state_other_slot about a code that is not
 *     yet known to be A or B, and the read-back compares the bytes read with the bytes written.
 * What cannot differ: the record (boot_state_t, common/boot_state.h), the CRC-32 (Interfaces
 * Sec. 4.0), the two copy addresses (layout.h) and decision D's three answers.
 *
 * Wear: each write erases one page. A page lasts 10 000 erases [DS12589 Rev 6, Table 47, p. 107]
 * and writes alternate between the two copies, so the state survives about 20 000 writes. The
 * same limit means the 32-bit sequence number never wraps.
 */
#include "bl_boot_state.h"

#include <stddef.h>

#include "bl_crc32.h"
#include "bl_flash.h"
#include "fault.h"
#include "layout.h"

/* Byte offsets of the fields in a stored record. The order is boot_state_t's (common/
 * boot_state.h): 4 + 4 x 1 + 4 + 4 bytes, no padding. */
#define AT_SEQUENCE        0U
#define AT_CONFIRMED       4U
#define AT_TRIAL           5U
#define AT_FAILED_STARTS   6U
#define AT_FLAGS           7U
#define AT_GIVEN_UP_CHECK  8U
#define AT_CHECK           12U
#define RECORD_BYTES       16U
/* The record is programmed as two 64-bit flash words of two 32-bit words each
 * [RM0440 Rev 9, Sec. 5.3.7, p. 196]. */
#define BYTES_PER_WORD     4U
#define BITS_PER_BYTE      8U
#define RECORD_WORDS       (RECORD_BYTES / BYTES_PER_WORD)
#define RECORD_PAIRS       (RECORD_WORDS / 2U)
/* Two stored copies (layout.h, BOOT_STATE_COPY_0 and _1). */
#define COPIES             2U
/* An erased flash byte reads 0xFF [RM0440 Rev 9, Sec. 5.3.7, p. 197]. Erased flash raises no
 * two-bit ECC error when read [RM0440 Rev 9, Sec. 5.3.2, p. 190, Note: for virgin data two-error
 * detection is not supported], so a record never written reads as all 0xFF, no ECC error. */
#define ERASED_BYTE        0xFFU
#define ERASED_WORD        0xFFFFFFFFU

/* The offsets above are boot_state_t's: the firmware writes the struct's memory, this file
 * writes the offsets, and both must give the same 16 bytes. */
_Static_assert(offsetof(boot_state_t, sequence) == AT_SEQUENCE, "sequence at byte 0");
_Static_assert(offsetof(boot_state_t, confirmed) == AT_CONFIRMED, "confirmed at byte 4");
_Static_assert(offsetof(boot_state_t, trial) == AT_TRIAL, "trial at byte 5");
_Static_assert(offsetof(boot_state_t, failed_starts) == AT_FAILED_STARTS, "starts at byte 6");
_Static_assert(offsetof(boot_state_t, flags) == AT_FLAGS, "flags at byte 7");
_Static_assert(offsetof(boot_state_t, given_up_check) == AT_GIVEN_UP_CHECK,
               "given_up_check at byte 8");
_Static_assert(offsetof(boot_state_t, check) == AT_CHECK, "check at byte 12");
_Static_assert(sizeof(boot_state_t) == RECORD_BYTES, "boot state is two 64-bit flash words");

/* Where each copy is stored, and where a write goes when that copy is the newest valid one. */
static const uint32_t COPY_ADDRESS[COPIES] = {BOOT_STATE_COPY_0, BOOT_STATE_COPY_1};
static const uint32_t OTHER_COPY_ADDRESS[COPIES] = {BOOT_STATE_COPY_1, BOOT_STATE_COPY_0};

/**
 * @brief Writes a 32-bit value as 4 bytes, least significant first.
 *
 * The chip stores a word with its least significant byte at the lowest address [RM0440 Rev 9,
 * Sec. 2.2.1, p. 82], and the firmware writes the record's words that way.
 *
 * @param[out]    bytes  first of the 4 bytes
 * @param[in]     value  the value
 */
static void put_u32(uint8_t *bytes, uint32_t value)
{
    for (uint32_t i = 0U; i < BYTES_PER_WORD; i++) {
        bytes[i] = (uint8_t)(value >> (i * BITS_PER_BYTE));
    }
}

/**
 * @brief Reads a 32-bit value from 4 bytes, least significant first (see put_u32).
 *
 * @param[in]     bytes  first of the 4 bytes
 * @return the value
 */
static uint32_t get_u32(const uint8_t *bytes)
{
    uint32_t value = 0U;

    for (uint32_t i = 0U; i < BYTES_PER_WORD; i++) {
        value |= (uint32_t)bytes[i] << (i * BITS_PER_BYTE);
    }
    return value;
}

/**
 * @brief Writes a record's fields at their offsets: the 16 bytes as they are stored in flash.
 *
 * @param[in]     state   the record
 * @param[out]    record  its 16 bytes
 */
static void encode(const boot_state_t *state, uint8_t record[RECORD_BYTES])
{
    put_u32(&record[AT_SEQUENCE], state->sequence);
    record[AT_CONFIRMED] = state->confirmed;
    record[AT_TRIAL] = state->trial;
    record[AT_FAILED_STARTS] = state->failed_starts;
    record[AT_FLAGS] = state->flags;
    put_u32(&record[AT_GIVEN_UP_CHECK], state->given_up_check);
    put_u32(&record[AT_CHECK], state->check);
}

/**
 * @brief Reads a record's fields from their offsets in the 16 stored bytes.
 *
 * The bytes are taken whatever they hold; the checks come after (intact, erased).
 *
 * @param[in]     record  the 16 bytes as stored
 * @param[out]    state   the record
 */
static void decode(const uint8_t record[RECORD_BYTES], boot_state_t *state)
{
    state->sequence = get_u32(&record[AT_SEQUENCE]);
    state->confirmed = record[AT_CONFIRMED];
    state->trial = record[AT_TRIAL];
    state->failed_starts = record[AT_FAILED_STARTS];
    state->flags = record[AT_FLAGS];
    state->given_up_check = get_u32(&record[AT_GIVEN_UP_CHECK]);
    state->check = get_u32(&record[AT_CHECK]);
}

/**
 * @brief Returns the check value a record must carry: the CRC-32 of its 12 encoded bytes before
 * the check field (REQ-136; Interfaces Sec. 4.0).
 *
 * @param[in]     state  the record
 * @return CRC-32 of bytes 0 to 11 of the encoded record
 */
static uint32_t check_value(const boot_state_t *state)
{
    uint8_t record[RECORD_BYTES];

    encode(state, record);
    return bl_crc32_compute(record, AT_CHECK);
}

/**
 * @brief Returns true if the slot fields, the start count and the flags of a record have allowed
 * values.
 *
 * Confirmed must be slot A or slot B; trial must be none or the slot that is not confirmed; the
 * start count cannot be above BOOT_STATE_MAX_STARTS, because main.c gives the trial up before it
 * would store a higher count (count_trial_start), so a higher stored count is damage. The flags
 * may hold no bit but BOOT_STATE_FLAG_FALLBACK, the only one any writer sets (owner's decision 30,
 * 15 Oct 2026): another bit is damage. Tested here as a mask, where common/boot_state.c compares
 * with the two allowed values. Every given_up_check value is allowed (a CRC-32; 0 means none).
 * A record can come from flash: a value that fails here is damage, judged by decision D, never
 * the fault reaction (owner's fault decision of 13 Oct 2026, point 4). So confirmed is tested
 * against A and B first, and only a confirmed slot known to be A or B is given to
 * bl_boot_state_other_slot, which treats any other code as a fault.
 *
 * @param[in]     state  the record
 * @return true if the slots, the count and the flags are allowed, false otherwise
 */
static bool fields_allowed(const boot_state_t *state)
{
    if ((state->confirmed != BOOT_SLOT_A) && (state->confirmed != BOOT_SLOT_B)) {
        return false;
    }
    if (state->failed_starts > BOOT_STATE_MAX_STARTS) {
        return false;
    }
    if (((uint32_t)state->flags & ~(uint32_t)BOOT_STATE_FLAG_FALLBACK) != 0U) {
        return false;
    }
    return (state->trial == BOOT_SLOT_NONE) ||
           (state->trial == bl_boot_state_other_slot(state->confirmed));
}

/**
 * @brief Returns true if a record passes its checks: allowed fields and the right check value
 * (REQ-136).
 *
 * @param[in]     state  the record, copied to RAM
 * @return true if both hold, false otherwise
 */
static bool intact(const boot_state_t *state)
{
    return fields_allowed(state) && (state->check == check_value(state));
}

/**
 * @brief Returns true if every field of a record holds the erased value: the copy was never
 * written since its last erase (decision D: "blank").
 *
 * @param[in]     state  the record, copied to RAM
 * @return true if all fields read as erased flash (16 bytes 0xFF), false otherwise
 */
static bool erased(const boot_state_t *state)
{
    return (state->sequence == ERASED_WORD) && (state->confirmed == ERASED_BYTE) &&
           (state->trial == ERASED_BYTE) && (state->failed_starts == ERASED_BYTE) &&
           (state->flags == ERASED_BYTE) && (state->given_up_check == ERASED_WORD) &&
           (state->check == ERASED_WORD);
}

/**
 * @brief Copies the 16 stored bytes of one copy to RAM, just before use (REQ-136), and reports a
 * flash read error.
 *
 * Read through bl_flash_at() so the host tests can serve a RAM copy of the flash. The ECC record
 * is cleared first, so an error seen afterwards belongs to these bytes.
 *
 * @param[in]     address  chip address of the copy (COPY_ADDRESS)
 * @param[out]    record   the 16 bytes as stored
 * @return true if the read saw no two-bit ECC error, false if it did (a power cut during
 *         programming can leave such a word [RM0440 Rev 9, Sec. 5.3.2, p. 190])
 */
static bool fetch(uint32_t address, uint8_t record[RECORD_BYTES])
{
    bl_flash_ecc_clear();
    const uint8_t *stored = bl_flash_at(address);
    for (uint32_t i = 0U; i < RECORD_BYTES; i++) {
        record[i] = stored[i];
    }
    return !bl_flash_ecc_error_seen();
}

/**
 * @brief Finds the newest good copy: the good one with the higher sequence number.
 *
 * One pass over the copies in order; a later copy replaces the one found only with a strictly
 * higher sequence number, so with equal numbers copy 0 is kept (as common/boot_state.c does:
 * the firmware and the bootloader then pick the same copy).
 *
 * @param[in]     copies  the two records
 * @param[in]     good    good[i] is true if copy i was read without error and is intact
 * @param[out]    newest  index of that copy, 0 or 1; written only when true is returned
 * @return true if a good copy exists, false otherwise
 */
static bool newest_good(const boot_state_t copies[COPIES], const bool good[COPIES],
                        uint32_t *newest)
{
    bool found = false;

    for (uint32_t c = 0U; c < COPIES; c++) {
        if (good[c] && (!found || (copies[c].sequence > copies[*newest].sequence))) {
            *newest = c;
            found = true;
        }
    }
    return found;
}

/**
 * @brief Decides what two records read from flash hold: a valid state, nothing yet, or damage
 * (decision D, owner, 13 Oct 2026).
 *
 * VALID: at least one copy was read without error and is intact; *state is the newest such
 * copy. BLANK: both copies read without error and every field is erased; main.c then uses the
 * default state (confirmed A, no trial). DAMAGED: neither copy is valid and at least one is not
 * blank, or a read saw a flash ECC error; main.c then starts only the last-resort firmware. Why
 * the difference: an erased record is what a fresh chip and a restore leave (Architecture
 * Sec. 5.1: the restore clears the kept data), so the default is right there; a written record
 * that fails its checks says the stored state is lost, and the default could start a slot the
 * operator had given up (REQ-019).
 *
 * @param[in]     copies    the two records as read
 * @param[in]     readable  readable[i] is false if reading copy i saw a flash ECC error
 * @param[out]    state     the newest valid copy; written only for BOOT_STATE_VALID
 * @return BOOT_STATE_VALID, BOOT_STATE_BLANK or BOOT_STATE_DAMAGED
 */
boot_state_load_t bl_boot_state_classify(const boot_state_t copies[COPIES],
                                         const bool readable[COPIES], boot_state_t *state)
{
    bool good[COPIES];
    bool all_blank = true;
    uint32_t newest = 0U;

    for (uint32_t c = 0U; c < COPIES; c++) {
        good[c] = readable[c] && intact(&copies[c]);
        all_blank = all_blank && readable[c] && erased(&copies[c]);
    }
    if (newest_good(copies, good, &newest)) {
        *state = copies[newest];
        return BOOT_STATE_VALID;
    }
    return all_blank ? BOOT_STATE_BLANK : BOOT_STATE_DAMAGED;
}

/**
 * @brief Reads both stored copies and classifies them (bl_boot_state_classify; decision D).
 *
 * @param[out]    state  the newest valid copy; written only for BOOT_STATE_VALID
 * @return BOOT_STATE_VALID, BOOT_STATE_BLANK or BOOT_STATE_DAMAGED
 */
boot_state_load_t bl_boot_state_load(boot_state_t *state)
{
    boot_state_t copies[COPIES];
    bool readable[COPIES];

    for (uint32_t c = 0U; c < COPIES; c++) {
        uint8_t record[RECORD_BYTES];
        readable[c] = fetch(COPY_ADDRESS[c], record);
        decode(record, &copies[c]);
    }
    return bl_boot_state_classify(copies, readable, state);
}

/**
 * @brief Reads the boot state into *state from the newest valid copy.
 *
 * The same reads and checks as bl_boot_state_load: a valid copy exists exactly when it answers
 * BOOT_STATE_VALID, and *state is then that copy.
 *
 * @param[out]    state  boot state read from flash; written only when true is returned
 * @return true if a valid copy was read, false if none is valid
 */
bool bl_boot_state_read(boot_state_t *state)
{
    return bl_boot_state_load(state) == BOOT_STATE_VALID;
}

/**
 * @brief Returns true if the copy at address now holds exactly the bytes of record and passes
 * the checks a later start will use.
 *
 * @param[in]     address  chip address of the copy just written
 * @param[in]     record   the 16 bytes that were programmed
 * @return true if read without error, equal byte for byte and intact; false otherwise
 */
static bool read_back_matches(uint32_t address, const uint8_t record[RECORD_BYTES])
{
    uint8_t stored[RECORD_BYTES];
    boot_state_t back;

    if (!fetch(address, stored)) {
        return false;
    }
    for (uint32_t i = 0U; i < RECORD_BYTES; i++) {
        if (stored[i] != record[i]) {
            return false;
        }
    }
    decode(stored, &back);
    return intact(&back);
}

/**
 * @brief Writes the boot state to the copy that is not the newest valid one, then reads it back.
 *
 * The fields are checked before any flash is read, so a record that is not allowed touches
 * nothing and *state is left as it is. Then: the newest good copy is found; the write goes to the
 * other copy, so the current one survives a power cut (REQ-017, code rule B7); with no good copy
 * it goes to copy 0, as common/boot_state.c does. The sequence number is one above the newest
 * (1 with none); flags and given_up_check are written as the caller set them (main.c copies
 * them from the record it loaded, or sets them for a give-up; until owner's decision 30, 15 Oct
 * 2026, they were "unused" and "reserved" and this function wrote them as 0); the check value is
 * computed last over the encoded bytes (data, then check value). The page is erased and the two
 * flash words programmed through bl_flash.c, whose barrier is checked before each operation. The
 * record sits at the start of its copy, so erasing the copy's first page is enough.
 *
 * @param[in,out] state  boot state to write; sequence and check are set
 * @return true if the state was written and read back equal; false if the state is not allowed
 *         (slots, start count or flags), or the erase, the programming or the read-back failed
 */
bool bl_boot_state_write(boot_state_t *state)
{
    boot_state_t copies[COPIES];
    bool good[COPIES];
    uint8_t record[RECORD_BYTES];
    uint32_t words[RECORD_WORDS];
    uint32_t newest = 0U;
    uint32_t target = BOOT_STATE_COPY_0;

    if (!fields_allowed(state)) {
        return false;
    }
    for (uint32_t c = 0U; c < COPIES; c++) {
        bool readable = fetch(COPY_ADDRESS[c], record);
        decode(record, &copies[c]);
        good[c] = readable && intact(&copies[c]);
    }
    state->sequence = 1U;
    if (newest_good(copies, good, &newest)) {
        target = OTHER_COPY_ADDRESS[newest];
        state->sequence = copies[newest].sequence + 1U;
    }
    state->check = check_value(state);
    encode(state, record);
    for (uint32_t w = 0U; w < RECORD_WORDS; w++) {
        words[w] = get_u32(&record[w * BYTES_PER_WORD]);
    }

    if (!bl_flash_erase_page(target) || !bl_flash_program(target, words, RECORD_PAIRS)) {
        return false;
    }
    return read_back_matches(target, record);
}

/**
 * @brief Returns slot B for slot A and slot A for slot B; any other code is a fault.
 *
 * The only caller, fields_allowed, asks only about a confirmed slot it has already found to be A
 * or B. So any other code here is a corrupted value in memory, and it gets the fault reaction
 * (common/fault.h; owner's fault decision of 13 Oct 2026) with this function's own
 * fault-location code (common/fault_codes.h, decision 3). A switch over the code, with no
 * default: only a corrupted value reaches the line after it (code rule A4, as main.c does for
 * slot codes; a deliberate deviation from MISRA C:2012 Rule 16.4, whose purpose the fault
 * reaction meets).
 *
 * @param[in]     slot  BOOT_SLOT_A or BOOT_SLOT_B
 * @return the other slot code
 * @note Does not return for any code but BOOT_SLOT_A or BOOT_SLOT_B.
 */
uint8_t bl_boot_state_other_slot(uint8_t slot)
{
    switch (slot) {
    case BOOT_SLOT_A:
        return BOOT_SLOT_B;
    case BOOT_SLOT_B:
        return BOOT_SLOT_A;
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_BL_BOOT_STATE_OTHER_SLOT, "bl_boot_state_other_slot");
}
