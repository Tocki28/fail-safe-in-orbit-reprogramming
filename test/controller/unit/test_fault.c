/*
 * test_fault.c — host unit tests of the bootloader's fault reaction (src/controller/bootloader/
 * fault.c), with its backup-register module (bootloader/backup.c) as built, against the RAM
 * stand-ins of RCC, PWR and TAMP that keep the real offsets (unit/backup_stubs). The UART and the
 * reset request are replaced here: the UART text is collected, and the reset request leaves the
 * code under test by longjmp.
 *
 * The rules under test (decision 3, owner, 13 Oct 2026):
 *   - the fault reaction leaves its fault-location code in the FAULT_LOCATION pair (TAMP_BKP10R,
 *     its complement in TAMP_BKP11R) and 0 in the FAULT_LATER pair (TAMP_BKP12R, BKP13R), with
 *     the backup domain opened by itself, before its log line, and touches no other backup
 *     register;
 *   - the FIRST location is kept: a second fault before a firmware reported leaves the location
 *     as it is and counts one in FAULT_LATER, which stops at 255; a broken FAULT_LATER pair
 *     counts from 0; a broken or empty location pair starts a new first fault (owner, 13 Oct
 *     2026, second round, decision 3);
 *   - the log line names the function and the code; then the reset is requested, once;
 *   - a fault found inside the backup module (a pair number outside the map) still leaves its own
 *     code and resets, once: the fault reaction does not call back into the module's pair-number
 *     check (no recursion).
 * Decision 7 (a fault loop inside the bootloader is not counted) holds because the reaction
 * writes no pair but FAULT_LOCATION and FAULT_LATER (the first test checks this) and the
 * bootloader clears its start record at every start (test_flow.c, decision B tests).
 * Fails on the old code: fault.c took no code and wrote nothing to the backup registers; the
 * keep-the-first and count tests fail on the code before the second round, which overwrote the
 * location at every fault and had no count (checked with a scratch copy of backup.c).
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer. Run: make -C test/controller
 * unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backup.h"
#include "backup_layout.h"
#include "fault.h"
#include "stm32g4xx.h"
#include "uart.h"

RCC_TypeDef fake_rcc;
PWR_TypeDef fake_pwr;
TAMP_TypeDef fake_tamp;

/* A value that no test writes, to see which registers were left alone. */
#define UNTOUCHED 0x5A5A5A5AU
/* Room for the UART text of one test. */
#define LOG_BYTES 256U
/* TAMP_BKP10R and TAMP_BKP11R: the FAULT_LOCATION pair (common/backup_layout.h). */
#define BKP_LOCATION     10U
#define BKP_LOCATION_INV 11U
/* TAMP_BKP12R and TAMP_BKP13R: the FAULT_LATER pair. */
#define BKP_LATER        12U
#define BKP_LATER_INV    13U

static uint32_t failures;
static uint32_t tests;
static jmp_buf reset_exit;
static bool reset_armed;
static volatile uint32_t resets;
static char log_text[LOG_BYTES];
static bool logged;
static uint32_t location_at_first_log;   /* TAMP_BKP10R when the first UART text was written */

/**
 * @brief Stand-in for the reset request: counts it and leaves by longjmp to reset_exit.
 *
 * @note Does not return; stops the test program if no test expected a reset.
 */
void NVIC_SystemReset(void)
{
    resets++;
    if (reset_armed) {
        reset_armed = false;
        longjmp(reset_exit, 1);
    }
    printf("FAIL  reset requested, and no test expected it\n");
    exit(1);
}

/**
 * @brief Stand-in for the UART: appends the text to log_text; notes TAMP_BKP10R at the first
 * call, so a test can see whether the code was left before the line.
 *
 * @param[in]     text  zero-terminated text
 */
void uart_put(const char *text)
{
    size_t used = strlen(log_text);

    if (!logged) {
        logged = true;
        location_at_first_log = fake_tamp.BKP[BKP_LOCATION];
    }
    if (used < (LOG_BYTES - 1U)) {
        (void)snprintf(&log_text[used], LOG_BYTES - used, "%s", text);   /* cut text is fine */
    }
}

/**
 * @brief Stand-in for the UART: appends "0x" and eight upper-case hex digits.
 *
 * @param[in]     value  number to write
 */
void uart_put_hex32(uint32_t value)
{
    char digits[16];

    (void)snprintf(digits, sizeof digits, "0x%08X", (unsigned)value);   /* 10 characters fit */
    uart_put(digits);
}

/**
 * @brief Stand-in for the UART: nothing to wait for on the host.
 */
void uart_wait_sent(void)
{
}

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
 * @brief Clears the fake registers (backup domain closed, every backup register UNTOUCHED), the
 * UART text and the reset count.
 */
static void reset_rig(void)
{
    memset(&fake_rcc, 0, sizeof fake_rcc);
    memset(&fake_pwr, 0, sizeof fake_pwr);
    memset(&fake_tamp, 0, sizeof fake_tamp);
    for (uint32_t i = 0U; i < FAKE_TAMP_BKP_REGISTERS; i++) {
        fake_tamp.BKP[i] = UNTOUCHED;
    }
    log_text[0] = '\0';
    logged = false;
    location_at_first_log = 0U;
    resets = 0U;
}

/**
 * @brief Returns true if no backup register but the FAULT_LOCATION and FAULT_LATER pairs was
 * touched.
 *
 * @return true if every other register still holds UNTOUCHED
 */
static bool others_untouched(void)
{
    bool others = true;

    for (uint32_t i = 0U; i < FAKE_TAMP_BKP_REGISTERS; i++) {
        if ((i != BKP_LOCATION) && (i != BKP_LOCATION_INV) && (i != BKP_LATER) &&
            (i != BKP_LATER_INV)) {
            others = others && (fake_tamp.BKP[i] == UNTOUCHED);
        }
    }
    return others;
}

/**
 * @brief The fault reaction with the backup domain closed: the code is left in BKP10R/BKP11R
 * before the log line, the line names the function and the code, one reset follows.
 */
static void test_reaction(void)
{
    uint32_t v = 0U;

    reset_rig();
    reset_armed = true;
    if (setjmp(reset_exit) == 0) {
        fault_impossible_value(FAULT_AT_SLOT_REGION, "slot_region");   /* must not come back */
    }
    report((resets == 1U) && (fake_tamp.BKP[BKP_LOCATION] == 0x05U) &&
               (fake_tamp.BKP[BKP_LOCATION_INV] == ~0x05U) && (location_at_first_log == 0x05U) &&
               others_untouched() && backup_read(BACKUP_PAIR_FAULT_LOCATION, &v) && (v == 0x05U),
           "3: fault in slot_region, backup domain closed -> 0x05 in BKP10R, complement in "
           "BKP11R, left before the log line; no other register touched; one reset");
    report((fake_tamp.BKP[BKP_LATER] == 0U) && (fake_tamp.BKP[BKP_LATER_INV] == 0xFFFFFFFFU) &&
               backup_read(BACKUP_PAIR_FAULT_LATER, &v) && (v == 0U),
           "3: a first fault sets the later-faults count to 0 (BKP12R = 0, BKP13R = complement)");
    report(strcmp(log_text, "BL: corrupted value in slot_region (fault location 0x00000005); "
                            "restarting\r\n") == 0,
           "3: log line names the function and the fault location");
}

/**
 * @brief A pair number outside the map found inside backup.c: the fault reaction leaves
 * value_index's code and resets once (no call back into value_index, no recursion).
 */
static void test_fault_inside_backup(void)
{
    volatile bool returned = false;

    reset_rig();
    reset_armed = true;
    if (setjmp(reset_exit) == 0) {
        (void)backup_write(BACKUP_PAIRS, 1U);   /* must not come back */
        returned = true;
    }
    report(!returned && (resets == 1U) && (fake_tamp.BKP[BKP_LOCATION] == FAULT_AT_VALUE_INDEX) &&
               (fake_tamp.BKP[BKP_LOCATION_INV] == ~(uint32_t)FAULT_AT_VALUE_INDEX) &&
               others_untouched() && (strstr(log_text, "backup pair number") != NULL),
           "3: backup_write(pair 7) -> fault in value_index, 0x01 left in BKP10R, one reset, "
           "nothing else written");
}

/**
 * @brief Runs the fault reaction once, with the reset caught.
 *
 * @param[in]     location  the fault-location code to report
 */
static void fault_once(uint8_t location)
{
    reset_armed = true;
    if (setjmp(reset_exit) == 0) {
        fault_impossible_value(location, "test");   /* must not come back */
    }
}

/**
 * @brief Returns the stored value of a pair, or 0xFFFFFFFF when the pair is broken.
 *
 * @param[in]     pair  BACKUP_PAIR_...
 * @return the value, or 0xFFFFFFFF
 */
static uint32_t pair_or_broken(uint32_t pair)
{
    uint32_t v = 0U;

    return backup_read(pair, &v) ? v : 0xFFFFFFFFU;
}

/**
 * @brief Decision 3 (second round): the first location is kept and the faults after it are
 * counted, saturating at 255.
 *
 * Fails on the old code: the second fault overwrote the location and nothing was counted.
 */
static void test_keep_first(void)
{
    reset_rig();
    fault_once(FAULT_AT_SLOT_REGION);
    fault_once(FAULT_AT_ROLE_TEXT);
    fault_once(FAULT_AT_LOAD_STATE);
    report((pair_or_broken(BACKUP_PAIR_FAULT_LOCATION) == FAULT_AT_SLOT_REGION) &&
               (pair_or_broken(BACKUP_PAIR_FAULT_LATER) == 2U) && (resets == 3U),
           "3: three faults in a row -> the first location 0x05 kept, 2 later faults counted");

    reset_rig();
    fault_once(FAULT_AT_SLOT_REGION);
    for (uint32_t i = 0U; i < 300U; i++) {
        fault_once(FAULT_AT_ROLE_TEXT);
    }
    report((pair_or_broken(BACKUP_PAIR_FAULT_LOCATION) == FAULT_AT_SLOT_REGION) &&
               (pair_or_broken(BACKUP_PAIR_FAULT_LATER) == BACKUP_FAULT_LATER_MAX),
           "3: 300 faults after the first -> the count stops at 255, the location stays");

    reset_rig();
    fault_once(FAULT_AT_SLOT_REGION);
    fake_tamp.BKP[BKP_LATER] ^= 0x01U;   /* one flipped bit: the count is broken */
    fault_once(FAULT_AT_ROLE_TEXT);
    report((pair_or_broken(BACKUP_PAIR_FAULT_LOCATION) == FAULT_AT_SLOT_REGION) &&
               (pair_or_broken(BACKUP_PAIR_FAULT_LATER) == 1U),
           "3: a broken count reads as 0 -> the next fault makes it 1, the location stays");

    reset_rig();
    fault_once(FAULT_AT_SLOT_REGION);
    fake_tamp.BKP[BKP_LOCATION] ^= 0x01U;   /* one flipped bit: the location is broken */
    fault_once(FAULT_AT_ROLE_TEXT);
    report((pair_or_broken(BACKUP_PAIR_FAULT_LOCATION) == FAULT_AT_ROLE_TEXT) &&
               (pair_or_broken(BACKUP_PAIR_FAULT_LATER) == 0U),
           "3: a broken location reads as empty -> the next fault is a new first fault, count 0");

    reset_rig();
    fault_once(FAULT_AT_SLOT_REGION);
    fault_once(FAULT_AT_ROLE_TEXT);
    fake_tamp.BKP[BKP_LOCATION] = 0U;   /* what a firmware's clear leaves: value 0 with ... */
    fake_tamp.BKP[BKP_LOCATION_INV] = 0xFFFFFFFFU;   /* ... its complement */
    fake_tamp.BKP[BKP_LATER] = 0U;
    fake_tamp.BKP[BKP_LATER_INV] = 0xFFFFFFFFU;
    fault_once(FAULT_AT_LOAD_STATE);
    report((pair_or_broken(BACKUP_PAIR_FAULT_LOCATION) == FAULT_AT_LOAD_STATE) &&
               (pair_or_broken(BACKUP_PAIR_FAULT_LATER) == 0U),
           "3: after the firmware cleared both pairs -> a new fault is a new first fault, count 0");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    test_reaction();
    test_fault_inside_backup();
    test_keep_first();
    if (failures == 0U) {
        printf("bootloader fault reaction unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("bootloader fault reaction unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
