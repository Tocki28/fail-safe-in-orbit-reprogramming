/*
 * test_flow.c — host unit tests of the order of the bootloader's steps (src/controller/
 * bootloader/main.c): the watchdog, the cycle-counter check, the start count and the park loop.
 *
 * The bootloader's own main.c is included, so the tests run the code that runs on the board,
 * main() included (renamed). The chip is replaced by unit/boot_stubs (UART text, clock, watchdog
 * and flash stand-ins, each adding one character to ev_trace). The boot state and the image
 * checks are replaced here by scripted stand-ins, because the real ones read the chip's flash
 * addresses; they also add to ev_trace. boot_state.c and image_check.c are not linked.
 *
 * The rules under test: the independent watchdog is started before anything else (decision A);
 * it is reloaded before each boot-state write and before each candidate's checks, and on every
 * pass of the park loop; a stored start count at or above the limit gives the trial up and
 * cannot wrap to 0 (audit finding #21); a cycle counter that does not count is logged.
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

/* Scripted boot state: what boot_state_read returns, and what boot_state_write was given. */
static boot_state_t stored;
static bool stored_valid;
static boot_state_t written[8];
static uint32_t write_count;
static bool write_succeeds;

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
 * @brief Stand-in for boot_state_read: returns the scripted state, or false if none is valid.
 *
 * @param[out]    state  the scripted state
 * @return stored_valid
 */
bool boot_state_read(boot_state_t *state)
{
    if (stored_valid) {
        *state = stored;
    }
    return stored_valid;
}

/**
 * @brief Stand-in for boot_state_write: records the record given and the event 'E'.
 *
 * @param[in,out] state  the record to write
 * @return write_succeeds
 */
bool boot_state_write(boot_state_t *state)
{
    ev_add('E');
    if (write_count < 8U) {
        written[write_count] = *state;
    }
    write_count++;
    return write_succeeds;
}

/**
 * @brief Stand-in for boot_state_other_slot (declared in boot_state.h, not used by main.c).
 *
 * @param[in]     slot  slot code
 * @return BOOT_SLOT_NONE
 */
uint8_t boot_state_other_slot(uint8_t slot)
{
    (void)slot;
    return BOOT_SLOT_NONE;
}

/**
 * @brief Stand-in for image_check_contents: records the event 'X' and refuses the image as empty.
 *
 * @param[in]     image         first byte of the image (not read)
 * @param[in]     region_start  region start (not used)
 * @param[in]     region_bytes  region size (not used)
 * @param[in]     kind          image kind (not used)
 * @param[out]    signed_bytes  not written
 * @return IMAGE_EMPTY
 */
image_result_t image_check_contents(const uint8_t *image, uint32_t region_start,
                                    uint32_t region_bytes, uint8_t kind, uint32_t *signed_bytes)
{
    (void)image;
    (void)region_start;
    (void)region_bytes;
    (void)kind;
    (void)signed_bytes;
    ev_add('X');
    return IMAGE_EMPTY;
}

/**
 * @brief Stand-in for image_check_signatures; never reached, because every image is refused first.
 *
 * @param[in]     image         not used
 * @param[in]     signed_bytes  not used
 * @return IMAGE_OK
 */
image_result_t image_check_signatures(const uint8_t *image, uint32_t signed_bytes)
{
    (void)image;
    (void)signed_bytes;
    return IMAGE_OK;
}

/**
 * @brief Stand-in for image_result_text.
 *
 * @param[in]     result  not used
 * @return "empty"
 */
const char *image_result_text(image_result_t result)
{
    (void)result;
    return "empty";
}

/**
 * @brief Stand-in for image_read_u32; never reached, because no image is accepted.
 *
 * @param[in]     bytes  not used
 * @return 0
 */
uint32_t image_read_u32(const uint8_t *bytes)
{
    (void)bytes;
    return 0U;
}

/**
 * @brief Clears the scripted state, the write record, the trace and the UART text.
 */
static void reset_rig(void)
{
    memset(&stored, 0, sizeof stored);
    stored_valid = true;
    memset(written, 0, sizeof written);
    write_count = 0U;
    write_succeeds = true;
    stub_cycles_counts = true;
    ev_clear();
    uart_log_clear();
}

/**
 * @brief Runs the whole bootloader main() on a trial that is refused, up to its park loop.
 *
 * The scripted state is confirmed A, trial B, 0 failed starts; every image is refused. The
 * park loop is left by the stand-in watchdog_reload (third reload after uart_off).
 */
static void run_main_to_park(void)
{
    stored.confirmed = BOOT_SLOT_A;
    stored.trial = BOOT_SLOT_B;
    stored.failed_starts = 0U;
    if (setjmp(park_exit) == 0) {
        bootloader_main();
    }
}

/**
 * @brief The order of the steps of main(): watchdog first; reloads before each write and each
 * candidate; reloads in the park loop.
 *
 * Trace: W C K U (start), R E (count the trial start), R X (trial B refused), R E (trial given
 * up), R X (slot A refused), R X (last-resort refused), O (UART off), R R R (park loop).
 * Before decision A there was no watchdog in the bootloader: no W and no R.
 */
static void test_main_order(void)
{
    reset_rig();
    run_main_to_park();
    report(strcmp(ev_trace, "WCKURERXRERXRXORRR") == 0,
           "main: W first, then C K U; R before each E and each X; park loop R R R");
    report(ev_trace[0] == 'W',
           "main: the watchdog is started before the cycle counter, the clock and the UART");
    report(strstr(uart_log, "no firmware can run") != NULL,
           "main: all images refused -> \"no firmware can run\", then the park loop");
}

/**
 * @brief A cycle counter that does not count is logged, and the bootloader carries on.
 */
static void test_cycles_not_counting(void)
{
    reset_rig();
    stub_cycles_counts = false;
    run_main_to_park();
    report((strstr(uart_log, "cycle counter not counting") != NULL) &&
               (strstr(uart_log, "no firmware can run") != NULL),
           "cycles_start() false -> logged \"cycle counter not counting\"; boot carries on");
}

/**
 * @brief count_trial_start compares before it increments (audit finding #21).
 *
 * A stored 255 would wrap to 0 when incremented and then pass the "> limit" test as if it were
 * a first start. Now any stored count at or above BOOT_STATE_MAX_STARTS gives the trial up.
 */
static void test_count_trial_start(void)
{
    boot_state_t state;

    reset_rig();
    memset(&state, 0, sizeof state);
    state.confirmed = BOOT_SLOT_A;
    state.trial = BOOT_SLOT_B;
    state.failed_starts = 255U;
    count_trial_start(&state);
    report((state.trial == BOOT_SLOT_NONE) && (state.failed_starts == 0U) &&
               (strstr(uart_log, "given up") != NULL) && (write_count == 1U) &&
               (written[0].trial == BOOT_SLOT_NONE),
           "count_trial_start: stored 255 -> trial given up, count 0, saved (no wrap to a start)");

    reset_rig();
    state.trial = BOOT_SLOT_B;
    state.failed_starts = (uint8_t)BOOT_STATE_MAX_STARTS;
    count_trial_start(&state);
    report((state.trial == BOOT_SLOT_NONE) && (strstr(uart_log, "given up") != NULL),
           "count_trial_start: stored 3 (the limit) -> trial given up");

    reset_rig();
    state.trial = BOOT_SLOT_B;
    state.failed_starts = (uint8_t)(BOOT_STATE_MAX_STARTS - 1U);
    count_trial_start(&state);
    report((state.trial == BOOT_SLOT_B) && (state.failed_starts == BOOT_STATE_MAX_STARTS) &&
               (write_count == 1U) && (written[0].failed_starts == BOOT_STATE_MAX_STARTS) &&
               (strstr(uart_log, "start 3 of 3") != NULL),
           "count_trial_start: stored 2 -> start 3 of 3 counted and saved, trial kept");

    reset_rig();
    state.trial = BOOT_SLOT_B;
    state.failed_starts = 0U;
    count_trial_start(&state);
    report((state.trial == BOOT_SLOT_B) && (state.failed_starts == 1U) && (write_count == 1U),
           "count_trial_start: stored 0 -> start 1 of 3 counted and saved");

    reset_rig();
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    count_trial_start(&state);
    report((write_count == 0U) && (state.failed_starts == 0U),
           "count_trial_start: no trial -> nothing counted, nothing saved");

    reset_rig();
    write_succeeds = false;
    state.trial = BOOT_SLOT_B;
    state.failed_starts = 0U;
    count_trial_start(&state);
    report((state.trial == BOOT_SLOT_NONE) &&
               (strstr(uart_log, "could not be counted") != NULL),
           "count_trial_start: the save fails -> trial not started, confirmed slot runs");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    test_main_order();
    test_cycles_not_counting();
    test_count_trial_start();
    if (failures == 0U) {
        printf("bootloader flow tests: all %u passed\n", tests);
        return 0;
    }
    printf("bootloader flow tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
