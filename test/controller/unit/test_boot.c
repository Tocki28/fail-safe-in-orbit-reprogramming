/*
 * test_boot.c — host unit tests of the bootloader's slot handling (src/controller/bootloader/
 * main.c: slot_region, candidates, slot_letter) and of boot_state_other_slot (src/controller/
 * common/boot_state.c), for slot codes that name no slot.
 *
 * The bootloader's own main.c is included here, so the tests call the code that runs on the
 * board, static functions and all. Its main() is renamed and never called. The chip is replaced
 * by unit/boot_stubs: ST's device header by a RAM register, the UART by a text buffer.
 *
 * The rule under test: a slot code that is not BOOT_SLOT_A or BOOT_SLOT_B names no slot. It was
 * taken for slot B (slot_region) or slot A (boot_state_other_slot); now it is left out of the
 * candidates, and the bootloader falls back to the last-resort firmware (REQ-019).
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
 * @brief slot_region: A and B give their regions; none and codes that name no slot give NULL.
 *
 * Before, every code that was not A gave the region of slot B.
 */
static void test_slot_region(void)
{
    report((slot_region(BOOT_SLOT_A) == &REGION_A) && (slot_region(BOOT_SLOT_B) == &REGION_B),
           "slot_region: A -> region A, B -> region B");
    report((slot_region(BOOT_SLOT_NONE) == NULL) && (slot_region(3U) == NULL) &&
               (slot_region(0xEEU) == NULL) && (slot_region(0xFFU) == NULL),
           "slot_region: none, 3, 0xEE and 0xFF -> NULL (not region B)");
}

/**
 * @brief Runs candidates() for one boot state and returns how many regions it lists.
 *
 * @param[in]     confirmed  confirmed slot code
 * @param[in]     trial      trial slot code
 * @param[out]    order      the regions listed
 * @return number of regions listed
 */
static uint32_t list_for(uint8_t confirmed, uint8_t trial, const region_t *order[MAX_CANDIDATES])
{
    boot_state_t state;

    memset(&state, 0, sizeof state);
    state.confirmed = confirmed;
    state.trial = trial;
    uart_log_clear();
    return candidates(&state, order);
}

/**
 * @brief candidates(): the order for valid states, and a code that names no slot is left out.
 *
 * Before, a damaged confirmed or trial code started slot B; now only the real slots and the
 * last-resort firmware are listed, and the log says which code was left out.
 */
static void test_candidates(void)
{
    const region_t *order[MAX_CANDIDATES];
    uint32_t n;

    n = list_for(BOOT_SLOT_A, BOOT_SLOT_NONE, order);
    report((n == 2U) && is_region(order[0], "slot A") && is_region(order[1], "last-resort"),
           "candidates: confirmed A, no trial -> slot A, last-resort");
    n = list_for(BOOT_SLOT_B, BOOT_SLOT_A, order);
    report((n == 3U) && is_region(order[0], "slot A") && is_region(order[1], "slot B") &&
               is_region(order[2], "last-resort"),
           "candidates: confirmed B, trial A -> slot A, slot B, last-resort");
    n = list_for(BOOT_SLOT_A, BOOT_SLOT_B, order);
    report((n == 3U) && is_region(order[0], "slot B") && is_region(order[1], "slot A") &&
               is_region(order[2], "last-resort"),
           "candidates: confirmed A, trial B -> slot B, slot A, last-resort");

    n = list_for(0xEEU, BOOT_SLOT_NONE, order);
    report((n == 1U) && is_region(order[0], "last-resort") &&
               (strstr(uart_log, "confirmed slot code names no slot") != NULL),
           "candidates: confirmed code 0xEE -> last-resort only (not slot B), logged");
    n = list_for(BOOT_SLOT_NONE, BOOT_SLOT_NONE, order);
    report((n == 1U) && is_region(order[0], "last-resort"),
           "candidates: confirmed code none -> last-resort only (not slot B)");
    n = list_for(BOOT_SLOT_A, 0xEEU, order);
    report((n == 2U) && is_region(order[0], "slot A") && is_region(order[1], "last-resort") &&
               (strstr(uart_log, "trial slot code names no slot") != NULL),
           "candidates: trial code 0xEE -> trial left out (not slot B), logged");
    n = list_for(3U, 0xFFU, order);
    report((n == 1U) && is_region(order[0], "last-resort"),
           "candidates: both codes name no slot -> last-resort only");
}

/**
 * @brief slot_letter: A, B and none have their own text; a code that is none of them is "unknown".
 *
 * Before, any other code was logged as "none", which reads as "no trial".
 */
static void test_slot_letter(void)
{
    report((strcmp(slot_letter(BOOT_SLOT_A), "A") == 0) &&
               (strcmp(slot_letter(BOOT_SLOT_B), "B") == 0) &&
               (strcmp(slot_letter(BOOT_SLOT_NONE), "none") == 0),
           "slot_letter: A, B and none have their own text");
    report((strcmp(slot_letter(3U), "unknown") == 0) && (strcmp(slot_letter(0xEEU), "unknown") == 0),
           "slot_letter: 3 and 0xEE -> \"unknown\" (not \"none\")");
}

/**
 * @brief boot_state_other_slot: A gives B, B gives A; none and other codes give none.
 *
 * Before, every code that was not A gave A.
 */
static void test_other_slot(void)
{
    report((boot_state_other_slot(BOOT_SLOT_A) == BOOT_SLOT_B) &&
               (boot_state_other_slot(BOOT_SLOT_B) == BOOT_SLOT_A),
           "boot_state_other_slot: A -> B, B -> A");
    report((boot_state_other_slot(BOOT_SLOT_NONE) == BOOT_SLOT_NONE) &&
               (boot_state_other_slot(3U) == BOOT_SLOT_NONE) &&
               (boot_state_other_slot(0xEEU) == BOOT_SLOT_NONE),
           "boot_state_other_slot: none, 3 and 0xEE -> none (not slot A)");
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
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    test_slot_region();
    test_candidates();
    test_slot_letter();
    test_other_slot();
    test_start_count_bound();
    if (failures == 0U) {
        printf("bootloader unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("bootloader unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
