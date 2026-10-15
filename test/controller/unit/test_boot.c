/*
 * test_boot.c — host unit tests of the bootloader's own functions (src/controller/bootloader/
 * main.c: slot_region, candidates, slot_letter, check_result_code, reset_cause_text, role_text)
 * and of the bootloader's own bl_boot_state.c (bl_boot_state_other_slot, the start-count bound,
 * and decision D's blank/damaged decision, boot_state_classify and boot_state_load).
 *
 * The bootloader's own main.c is included here, so the tests call the code that runs on the
 * board, static functions and all. Its main() is renamed and never called. The chip is replaced
 * by unit/boot_stubs: ST's device header by a RAM register, the UART by a text buffer, the flash
 * by a RAM copy behind flash_at(), the fault reaction by a stand-in that records the call and
 * leaves by longjmp.
 *
 * The rules under test:
 *   - a value in memory that cannot exist is a fault (owner's decision, 13 Oct 2026): every
 *     function above that switches over a slot code or an enum calls the fault reaction for a
 *     value outside its cases, and does nothing else (no log line of a normal value, no region);
 *   - a value read from flash that fails its check is damage, never a fault (point 4 of the same
 *     decision): boot_state_classify answers DAMAGED;
 *   - decision D: both copies erased -> BLANK; a copy written but not valid, with no valid copy ->
 *     DAMAGED;
 *   - the check-result codes of Interfaces Sec. 5.4 ("Check result" table);
 *   - decision 3: each fault site passes its function's fault-location code
 *     (common/fault_codes.h), checked by faulted_only.
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer. Run: make -C test/controller
 * unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#define main bootloader_main
#include "../../../src/controller/bootloader/main.c"
#undef main

#include <stdio.h>
#include <string.h>

#include "boot_stubs.h"
#include "crc32.h"

static uint32_t failures;
static uint32_t tests;

/**
 * @brief Counts one test result and prints a PASS or FAIL line for it.
 *
 * @param[in]     ok    true if the check passed
 * @param[in]     name  text printed after PASS or FAIL
 */
static void report(bool ok, const char *name)
{
    tests++;
    if (!ok) {
        failures++;
    }
    printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief Clears the fault record and the UART text before a call that may fault.
 */
static void fresh(void)
{
    fault_clear();
    uart_log_clear();
}

/**
 * @brief Returns true if a run ended in the fault reaction, called once at `where` with its
 * fault-location code (decision 3, common/fault_codes.h), with nothing logged before it.
 *
 * @param[in]     ended     how the run ended (RUN_CATCH)
 * @param[in]     where     the function name the fault reaction must have been given
 * @param[in]     location  the fault-location code it must have been given
 * @return true if so
 */
static bool faulted_only(int ended, const char *where, uint8_t location)
{
    return (ended == RUN_FAULT) && (fault_calls == 1U) && (strcmp(fault_where, where) == 0) &&
           (fault_location == location) && (uart_log[0] == '\0');
}

/**
 * @brief Returns true if a region is the one with the given name.
 *
 * @param[in]     region  region to look at; may be NULL
 * @param[in]     name    expected name: "slot A", "slot B" or "last-resort"
 * @return true if region is not NULL and has that name
 */
static bool is_region(const region_t *region, const char *name)
{
    return (region != NULL) && (strcmp(region->name, name) == 0);
}

/**
 * @brief slot_region: A and B give their regions; any other code is the fault reaction.
 *
 * Before (until this change): NULL for none and for codes that name no slot, and the caller left
 * them out; before that, slot B. Fails on the old code: no fault reaction is called.
 */
static void test_slot_region(void)
{
    const uint8_t bad[] = {BOOT_SLOT_NONE, 3U, 0xEEU, 0xFFU};
    bool all = true;

    report((slot_region(BOOT_SLOT_A) == &REGION_A) && (slot_region(BOOT_SLOT_B) == &REGION_B),
           "slot_region: A -> region A, B -> region B");
    for (uint32_t i = 0U; i < (uint32_t)(sizeof bad / sizeof bad[0]); i++) {
        const region_t *volatile got = NULL;
        int ended = 0;

        fresh();
        RUN_CATCH(ended, got = slot_region(bad[i]));
        all = all && faulted_only(ended, "slot_region", FAULT_AT_SLOT_REGION) && (got == NULL);
    }
    report(all, "slot_region: none, 3, 0xEE, 0xFF -> fault reaction, no region returned (was "
                "NULL)");
}

/**
 * @brief Runs candidates() for one boot state and returns how many regions it lists.
 *
 * @param[in]     confirmed  confirmed slot code
 * @param[in]     trial      trial slot code
 * @param[in]     skip       skip_confirmed (decision B)
 * @param[out]    order      the regions listed
 * @return number of regions listed
 */
static uint32_t list_for(uint8_t confirmed, uint8_t trial, bool skip,
                         const region_t *order[MAX_CANDIDATES])
{
    boot_state_t state;

    memset(&state, 0, sizeof state);
    state.confirmed = confirmed;
    state.trial = trial;
    return candidates(&state, skip, order);
}

/**
 * @brief candidates(): the order for valid states, decision B's skip, and a code that names no
 * slot is a fault.
 *
 * Before, a damaged confirmed or trial code was left out with a log line, and the list went on
 * (last-resort only). Fails on the old code: no fault reaction; the skip parameter did not exist.
 */
static void test_candidates(void)
{
    const region_t *order[MAX_CANDIDATES];
    uint32_t n;
    int ended = 0;

    n = list_for(BOOT_SLOT_A, BOOT_SLOT_NONE, false, order);
    report((n == 2U) && is_region(order[0], "slot A") && is_region(order[1], "last-resort"),
           "candidates: confirmed A, no trial -> slot A, last-resort");
    n = list_for(BOOT_SLOT_B, BOOT_SLOT_A, false, order);
    report((n == 3U) && is_region(order[0], "slot A") && is_region(order[1], "slot B") &&
               is_region(order[2], "last-resort"),
           "candidates: confirmed B, trial A -> slot A, slot B, last-resort");
    n = list_for(BOOT_SLOT_A, BOOT_SLOT_B, false, order);
    report((n == 3U) && is_region(order[0], "slot B") && is_region(order[1], "slot A") &&
               is_region(order[2], "last-resort"),
           "candidates: confirmed A, trial B -> slot B, slot A, last-resort");
    n = list_for(BOOT_SLOT_A, BOOT_SLOT_NONE, true, order);
    report((n == 1U) && is_region(order[0], "last-resort"),
           "candidates, decision B: confirmed A skipped, no trial -> last-resort only");
    n = list_for(BOOT_SLOT_A, BOOT_SLOT_B, true, order);
    report((n == 2U) && is_region(order[0], "slot B") && is_region(order[1], "last-resort"),
           "candidates, decision B: confirmed A skipped, trial B -> slot B, last-resort");

    volatile uint32_t got = 99U;
    fresh();
    RUN_CATCH(ended, got = list_for(0xEEU, BOOT_SLOT_NONE, false, order));
    report(faulted_only(ended, "slot_region", FAULT_AT_SLOT_REGION) && (got == 99U),
           "candidates: confirmed code 0xEE -> fault reaction, no list (was: last-resort only)");
    fresh();
    RUN_CATCH(ended, got = list_for(BOOT_SLOT_NONE, BOOT_SLOT_NONE, false, order));
    report(faulted_only(ended, "slot_region", FAULT_AT_SLOT_REGION) && (got == 99U),
           "candidates: confirmed code none -> fault reaction (was: last-resort only)");
    fresh();
    RUN_CATCH(ended, got = list_for(BOOT_SLOT_A, 0xEEU, false, order));
    report(faulted_only(ended, "slot_region", FAULT_AT_SLOT_REGION) && (got == 99U),
           "candidates: trial code 0xEE -> fault reaction (was: trial left out, slot A, "
           "last-resort)");
}

/**
 * @brief slot_letter: A, B and none have their own text; any other code is the fault reaction.
 *
 * Before: "corrupted slot code" (b18e3e9), and "unknown" before that, logged as if it were data.
 * Fails on the old code: it returned a text.
 */
static void test_slot_letter(void)
{
    const uint8_t bad[] = {3U, 0xEEU};
    bool all = true;

    report((strcmp(slot_letter(BOOT_SLOT_A), "A") == 0) &&
               (strcmp(slot_letter(BOOT_SLOT_B), "B") == 0) &&
               (strcmp(slot_letter(BOOT_SLOT_NONE), "none") == 0),
           "slot_letter: A, B and none have their own text");
    for (uint32_t i = 0U; i < 2U; i++) {
        const char *volatile text = NULL;
        int ended = 0;

        fresh();
        RUN_CATCH(ended, text = slot_letter(bad[i]));
        all = all && faulted_only(ended, "slot_letter", FAULT_AT_SLOT_LETTER) && (text == NULL);
    }
    report(all, "slot_letter: 3 and 0xEE -> fault reaction, no text (was \"corrupted slot code\")");
}

/**
 * @brief bl_boot_state_other_slot (the bootloader's own, via BL_ALIAS): A gives B, B gives A;
 * none and other codes are the fault reaction.
 *
 * Since decision 35 (REQ-119) the bootloader's main.c uses bl_boot_state.c, so its fault site is
 * "bl_boot_state_other_slot" with FAULT_AT_BL_BOOT_STATE_OTHER_SLOT, not the firmware's
 * "boot_state_other_slot" / FAULT_AT_BOOT_STATE_OTHER_SLOT. Same inputs, same strictness.
 * Before: BOOT_SLOT_NONE for them, and slot A before that. Fails on the old code: no fault.
 */
static void test_other_slot(void)
{
    const uint8_t bad[] = {BOOT_SLOT_NONE, 3U, 0xEEU};
    bool all = true;

    report((boot_state_other_slot(BOOT_SLOT_A) == BOOT_SLOT_B) &&
               (boot_state_other_slot(BOOT_SLOT_B) == BOOT_SLOT_A),
           "boot_state_other_slot: A -> B, B -> A");
    for (uint32_t i = 0U; i < 3U; i++) {
        volatile uint8_t got = 0x55U;
        int ended = 0;

        fresh();
        RUN_CATCH(ended, got = boot_state_other_slot(bad[i]));
        all = all &&
              faulted_only(ended, "bl_boot_state_other_slot", FAULT_AT_BL_BOOT_STATE_OTHER_SLOT) &&
              (got == 0x55U);
    }
    report(all, "bl_boot_state_other_slot: none, 3, 0xEE -> fault reaction (was: none)");
}

/**
 * @brief boot_state_write refuses a record whose start count is above BOOT_STATE_MAX_STARTS.
 *
 * The check is made before any flash is read, so the host never touches the chip's addresses.
 * Before, fields_valid looked at the slots only: a count of 255 was written, and read back as
 * valid. (Before the check also moved ahead of newest_copy(), this call read the flash address
 * and stopped the host test with a memory fault.)
 */
static void test_start_count_bound(void)
{
    boot_state_t state;

    report(BOOT_STATE_MAX_STARTS == 3U, "BOOT_STATE_MAX_STARTS is 3 (bench value, REQ-015)");
    memset(&state, 0, sizeof state);
    state.confirmed = BOOT_SLOT_A;
    state.trial = BOOT_SLOT_B;
    state.failed_starts = (uint8_t)(BOOT_STATE_MAX_STARTS + 1U);
    report(!boot_state_write(&state), "boot_state_write: failed starts 4 -> refused (false)");
    state.failed_starts = 255U;
    report(!boot_state_write(&state), "boot_state_write: failed starts 255 -> refused (false)");
}

/**
 * @brief Returns a boot-state record with a right check value (CRC-32 of its first 12 bytes,
 * common/boot_state.h).
 *
 * @param[in]     sequence   sequence number
 * @param[in]     confirmed  confirmed slot code
 * @param[in]     trial      trial slot code
 * @param[in]     starts     failed starts
 * @return the record
 */
static boot_state_t record_of(uint32_t sequence, uint8_t confirmed, uint8_t trial, uint8_t starts)
{
    boot_state_t r;

    memset(&r, 0, sizeof r);
    r.sequence = sequence;
    r.confirmed = confirmed;
    r.trial = trial;
    r.failed_starts = starts;
    r.check = crc32_compute((const uint8_t *)&r, (uint32_t)offsetof(boot_state_t, check));
    return r;
}

/**
 * @brief Returns a record as erased flash leaves it: 16 bytes 0xFF.
 *
 * @return the record
 */
static boot_state_t erased_record(void)
{
    boot_state_t r;

    memset(&r, 0xFF, sizeof r);
    return r;
}

/**
 * @brief Classifies two records, both read without a flash error.
 *
 * @param[in]     c0   copy 0
 * @param[in]     c1   copy 1
 * @param[out]    out  the state, for BOOT_STATE_VALID
 * @return what boot_state_classify says
 */
static boot_state_load_t classify2(boot_state_t c0, boot_state_t c1, boot_state_t *out)
{
    const boot_state_t copies[2] = {c0, c1};
    const bool readable[2] = {true, true};

    return boot_state_classify(copies, readable, out);
}

/**
 * @brief Decision D on records in RAM: blank, damaged, valid; and a damaged field from flash is
 * damage, not a fault.
 *
 * Before decision D the bootloader could not tell blank from damaged (boot_state_read gave false
 * for both, and both got the default state). Fails on the old code: boot_state_classify did not
 * exist.
 */
static void test_classify(void)
{
    boot_state_t out;
    boot_state_t bad_crc = record_of(5U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    int ended = 0;

    bad_crc.check ^= 1U;
    report(classify2(erased_record(), erased_record(), &out) == BOOT_STATE_BLANK,
           "D: both copies erased (16 x 0xFF) -> BLANK (default state)");
    report(classify2(erased_record(), bad_crc, &out) == BOOT_STATE_DAMAGED,
           "D: one copy erased, the other written with a wrong check value -> DAMAGED");
    report(classify2(bad_crc, bad_crc, &out) == BOOT_STATE_DAMAGED,
           "D: both copies written, both failing their check -> DAMAGED");

    boot_state_t good = record_of(7U, BOOT_SLOT_B, BOOT_SLOT_A, 2U);
    memset(&out, 0, sizeof out);
    report((classify2(erased_record(), good, &out) == BOOT_STATE_VALID) &&
               (memcmp(&out, &good, sizeof out) == 0),
           "D: one copy erased, the other valid -> VALID with that copy");
    boot_state_t older = record_of(6U, BOOT_SLOT_A, BOOT_SLOT_NONE, 0U);
    memset(&out, 0, sizeof out);
    report((classify2(bad_crc, older, &out) == BOOT_STATE_VALID) && (out.sequence == 6U) &&
               (classify2(older, good, &out) == BOOT_STATE_VALID) && (out.sequence == 7U),
           "D: one damaged and one valid -> VALID; two valid -> the higher sequence number");

    const boot_state_t copies[2] = {erased_record(), erased_record()};
    const bool ecc_on_0[2] = {false, true};
    report(boot_state_classify(copies, ecc_on_0, &out) == BOOT_STATE_DAMAGED,
           "D: both erased, but copy 0 read with a flash ECC error -> DAMAGED, not BLANK");

    /* A record from flash with a right check value but a confirmed code that names no slot. */
    boot_state_t odd = record_of(9U, 0xEEU, BOOT_SLOT_NONE, 0U);
    volatile boot_state_load_t got = BOOT_STATE_VALID;
    fresh();
    RUN_CATCH(ended, got = classify2(odd, erased_record(), &out));
    report((ended == 0) && (got == BOOT_STATE_DAMAGED) && (fault_calls == 0U),
           "a record from flash with confirmed 0xEE (check value right) -> DAMAGED, no fault "
           "(flash data is checked, not a fault; fields_valid checks confirmed first)");
}

/**
 * @brief Decision D through boot_state_load on the RAM flash (flash_at): the real reads of
 * common/boot_state.c.
 *
 * Fails on the old code: boot_state_load did not exist (boot_state_read gave false for both the
 * blank and the damaged case).
 */
static void test_load_from_flash(void)
{
    const uint32_t copy1 = (uint32_t)BOOT_STATE_COPY_1 - (uint32_t)FLASH_START;
    boot_state_t out;
    boot_state_t good = record_of(3U, BOOT_SLOT_A, BOOT_SLOT_B, 1U);

    stub_flash_reset();
    report(boot_state_load(&out) == BOOT_STATE_BLANK,
           "D, from flash: both copies erased -> BLANK");
    memcpy(&stub_flash[copy1], &good, sizeof good);
    memset(&out, 0, sizeof out);
    report((boot_state_load(&out) == BOOT_STATE_VALID) && (out.confirmed == BOOT_SLOT_A) &&
               (out.trial == BOOT_SLOT_B) && (out.failed_starts == 1U),
           "D, from flash: copy 1 valid -> VALID, confirmed A, trial B, 1 start");
    stub_flash[copy1 + 4U] = 0x00U;   /* the confirmed byte: the check value no longer matches */
    report(boot_state_load(&out) == BOOT_STATE_DAMAGED,
           "D, from flash: copy 1 written but damaged, copy 0 erased -> DAMAGED");
    stub_flash_reset();
    stub_ecc_address = BOOT_STATE_COPY_0;
    report(boot_state_load(&out) == BOOT_STATE_DAMAGED,
           "D, from flash: both erased, copy 0 read with an ECC error -> DAMAGED");
    stub_flash_reset();
}

/**
 * @brief check_result_code: the 11 image check results give the 11 codes of the Interfaces
 * "Check result" table; a value outside the enum is the fault reaction.
 *
 * The expected codes are written from the Interfaces Sec. 5.4 table, not from main.c's macros.
 * Fails on the old code: the function did not exist.
 */
static void test_check_result_code(void)
{
    struct {
        image_result_t result;
        uint8_t code;
    } const cases[] = {
        {IMAGE_OK, 0x00U},
        {IMAGE_EMPTY, 0x01U},
        {IMAGE_BAD_MAGIC, 0x02U},
        {IMAGE_BAD_FORMAT, 0x03U},
        {IMAGE_BAD_KIND, 0x04U},
        {IMAGE_BAD_TARGET, 0x05U},
        {IMAGE_BAD_LENGTH, 0x06U},
        {IMAGE_BAD_START, 0x07U},
        {IMAGE_BAD_BODY_CHECK, 0x08U},
        {IMAGE_BAD_SIGNATURE_A, 0x09U},
        {IMAGE_BAD_SIGNATURE_B, 0x0AU},
    };
    const int outside[] = {11, 15};
    bool ok = true;
    bool all = true;

    for (uint32_t i = 0U; i < (uint32_t)(sizeof cases / sizeof cases[0]); i++) {
        ok = ok && (check_result_code(cases[i].result) == cases[i].code);
    }
    report(ok, "check_result_code: the 11 results -> 0x00 to 0x0A (Interfaces Sec. 5.4 table)");
    for (uint32_t i = 0U; i < 2U; i++) {
        volatile uint8_t got = 0x55U;
        int ended = 0;

        fresh();
        RUN_CATCH(ended, got = check_result_code((image_result_t)outside[i]));
        all = all && faulted_only(ended, "check_result_code", FAULT_AT_CHECK_RESULT_CODE) &&
              (got == 0x55U);
    }
    report(all, "check_result_code: a value outside image_result_t (11, 15) -> fault reaction");
}

/**
 * @brief reset_cause_text and role_text: every value has its text; a corrupted value is the fault
 * reaction. Both are new with this change (no old code to fail).
 */
static void test_texts(void)
{
    const char *volatile text = NULL;
    int ended = 0;

    report((strcmp(reset_cause_text(RESET_WATCHDOG), "watchdog") == 0) &&
               (strcmp(reset_cause_text(RESET_POWER_ON), "power-on") == 0) &&
               (strcmp(reset_cause_text(RESET_SOFTWARE), "software") == 0) &&
               (strcmp(reset_cause_text(RESET_PIN), "reset pin") == 0) &&
               (strcmp(reset_cause_text(RESET_OTHER), "other") == 0) &&
               (strcmp(role_text(BACKUP_ROLE_CONFIRMED), " (confirmed)") == 0) &&
               (strcmp(role_text(BACKUP_ROLE_TRIAL), " (trial)") == 0) &&
               (strcmp(role_text(BACKUP_ROLE_LAST_RESORT), " (last resort)") == 0),
           "reset_cause_text and role_text: every value has its own text");
    fresh();
    RUN_CATCH(ended, text = reset_cause_text((reset_cause_t)9));
    bool cause_fault =
        faulted_only(ended, "reset_cause_text", FAULT_AT_RESET_CAUSE_TEXT) && (text == NULL);
    fresh();
    RUN_CATCH(ended, text = role_text(7U));
    report(cause_fault && faulted_only(ended, "role_text", FAULT_AT_ROLE_TEXT) && (text == NULL),
           "reset_cause_text(9) and role_text(7): fault reaction, no text");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    stub_flash_reset();
    test_slot_region();
    test_candidates();
    test_slot_letter();
    test_other_slot();
    test_start_count_bound();
    test_classify();
    test_load_from_flash();
    test_check_result_code();
    test_texts();
    if (failures == 0U) {
        printf("bootloader unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("bootloader unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
