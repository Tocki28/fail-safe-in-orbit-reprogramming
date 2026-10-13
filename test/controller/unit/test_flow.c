/*
 * test_flow.c — host unit tests of the bootloader's whole start (src/controller/bootloader/
 * main.c, main()): the order of its steps and the owner's decisions of 13 Oct 2026 that act on
 * the start.
 *
 * The bootloader's own main.c is included, so the tests run the code that runs on the board,
 * main() included (renamed). The chip is replaced by unit/boot_stubs (UART text, clock, watchdog,
 * flash, TAMP backup registers as RAM, hand-over and fault reaction that leave the run by
 * longjmp). The boot state and the image checks are replaced here by scripted stand-ins:
 * boot_state_load answers a scripted case, boot_state_write a scripted result per call, and the
 * image checks a scripted result per region. boot_state.c and image_check.c are not linked.
 *
 * The rules under test:
 *   - the watchdog is started before anything else (decision A), reloaded before each boot-state
 *     write, each candidate's checks and on every pass of the park loop;
 *   - a stored start count at or above the limit gives the trial up and cannot wrap to 0 (audit
 *     finding #21); a cycle counter that does not count is logged;
 *   - B: watchdog restarts, and software restarts marked as a fault reaction, of a firmware
 *     started as the confirmed slot are counted per slot in the backup registers; at 3 the
 *     confirmed slot is skipped and the last-resort firmware starts;
 *   - D: a blank boot state gets the default; a damaged one starts only the last-resort firmware;
 *   - F(b): no trial start without a running watchdog; the start is then not counted;
 *   - G: the firmware's give-up request is applied before the start is counted;
 *   - I: the default-state and give-up saves are tried twice, with the result logged;
 *   - check-result reporting: the copy refused before the one started, its check result and the
 *     started copy are left in the backup registers.
 * Each test's comment says whether it failed on the code before this change.
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

#define WRITES_MAX 8U
/* Region indexes of the scripted image results. */
#define AT_A    0U
#define AT_B    1U
#define AT_LR   2U
/* Vector-table addresses the hand-over is given (layout.h). */
#define VECTOR_A   ((uint32_t)SLOT_A_START + (uint32_t)IMAGE_CODE_OFFSET)
#define VECTOR_B   ((uint32_t)SLOT_B_START + (uint32_t)IMAGE_CODE_OFFSET)
#define VECTOR_LR  ((uint32_t)LAST_RESORT_START + (uint32_t)IMAGE_CODE_OFFSET)

static uint32_t failures;
static uint32_t tests;

/* Scripted boot state: what boot_state_load answers, and what boot_state_write was given. */
static boot_state_t stored;
static int stored_load;   /* a boot_state_load_t value, or a corrupted one */
static boot_state_t written[WRITES_MAX];
static bool write_results[WRITES_MAX];
static uint32_t write_count;
/* Scripted image results per region (AT_A, AT_B, AT_LR). */
static image_result_t region_result[3];

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
 * @brief Stand-in for boot_state_load: the scripted case, and the scripted state when VALID.
 *
 * @param[out]    state  the scripted state, for BOOT_STATE_VALID
 * @return stored_load (a corrupted value too, for the fault test)
 */
boot_state_load_t boot_state_load(boot_state_t *state)
{
    if (stored_load == (int)BOOT_STATE_VALID) {
        *state = stored;
    }
    return (boot_state_load_t)stored_load;
}

/**
 * @brief Stand-in for boot_state_write: records the record and the event 'E'; answers the
 * scripted result of this call (true after the script ends).
 *
 * @param[in,out] state  the record to write
 * @return the scripted result
 */
bool boot_state_write(boot_state_t *state)
{
    bool ok = true;

    ev_add('E');
    if (write_count < WRITES_MAX) {
        written[write_count] = *state;
        ok = write_results[write_count];
    }
    write_count++;
    return ok;
}

/**
 * @brief Returns the scripted-result index of a region start.
 *
 * @param[in]     start  region start address
 * @return AT_A, AT_B or AT_LR
 */
static uint32_t region_index(uint32_t start)
{
    if (start == (uint32_t)SLOT_A_START) {
        return AT_A;
    }
    return (start == (uint32_t)SLOT_B_START) ? AT_B : AT_LR;
}

/**
 * @brief Stand-in for image_check_contents: records 'X' and answers the region's script; a
 * scripted signature result passes here and fails in image_check_signatures. A read of the
 * address stub_ecc_address raises the ECC flag, as a two-bit error during the checks would.
 *
 * @param[in]     image         first byte of the image (served by flash_at; not read)
 * @param[in]     region_start  region start, selects the script
 * @param[in]     region_bytes  region size (not used)
 * @param[in]     kind          image kind (not used)
 * @param[out]    signed_bytes  set to the header size when the contents pass
 * @return the scripted result, or IMAGE_OK for a scripted signature failure
 */
image_result_t image_check_contents(const uint8_t *image, uint32_t region_start,
                                    uint32_t region_bytes, uint8_t kind, uint32_t *signed_bytes)
{
    image_result_t r = region_result[region_index(region_start)];

    (void)image;
    (void)region_bytes;
    (void)kind;
    ev_add('X');
    if (region_start == stub_ecc_address) {
        (void)flash_at(region_start);   /* the read that meets the scripted ECC error */
    }
    if ((r == IMAGE_BAD_SIGNATURE_A) || (r == IMAGE_BAD_SIGNATURE_B)) {
        r = IMAGE_OK;
    }
    if (r == IMAGE_OK) {
        *signed_bytes = (uint32_t)IMAGE_HEADER_BYTES;
    }
    return r;
}

/**
 * @brief Stand-in for image_check_signatures: the region's scripted signature result.
 *
 * @param[in]     image         first byte of the image, a pointer into stub_flash
 * @param[in]     signed_bytes  not used
 * @return the scripted result if it is a signature failure, otherwise IMAGE_OK
 */
image_result_t image_check_signatures(const uint8_t *image, uint32_t signed_bytes)
{
    uint32_t start = (uint32_t)FLASH_START + (uint32_t)(image - stub_flash);
    image_result_t r = region_result[region_index(start)];

    (void)signed_bytes;
    return ((r == IMAGE_BAD_SIGNATURE_A) || (r == IMAGE_BAD_SIGNATURE_B)) ? r : IMAGE_OK;
}

/**
 * @brief Stand-in for image_result_text.
 *
 * @param[in]     result  not used
 * @return "scripted"
 */
const char *image_result_text(image_result_t result)
{
    (void)result;
    return "scripted";
}

/**
 * @brief Stand-in for image_read_u32: little-endian read.
 *
 * @param[in]     bytes  first of 4 bytes
 * @return the value
 */
uint32_t image_read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

/**
 * @brief Clears every script, the stand-ins and the logs: boot state confirmed A, no trial,
 * valid; every image passes; reset cause none; backup registers empty; watchdog running.
 */
static void reset_rig(void)
{
    memset(&stored, 0, sizeof stored);
    stored.confirmed = BOOT_SLOT_A;
    stored.trial = BOOT_SLOT_NONE;
    stored_load = (int)BOOT_STATE_VALID;
    memset(written, 0, sizeof written);
    for (uint32_t i = 0U; i < WRITES_MAX; i++) {
        write_results[i] = true;
    }
    write_count = 0U;
    region_result[AT_A] = IMAGE_OK;
    region_result[AT_B] = IMAGE_OK;
    region_result[AT_LR] = IMAGE_OK;
    fake_rcc.CSR = 0U;
    stub_cycles_counts = true;
    stub_watchdog_running = true;
    started_vector = 0U;
    stub_backup_reset();
    stub_flash_reset();
    fault_clear();
    ev_clear();
    uart_log_clear();
}

/**
 * @brief Runs the whole bootloader main() until it starts a firmware, parks, or faults.
 *
 * @return RUN_STARTED, RUN_PARKED or RUN_FAULT
 */
static int run_boot(void)
{
    int ended = 0;

    RUN_CATCH(ended, (void)bootloader_main());
    return ended;
}

/**
 * @brief Returns the value of a backup pair, or 0xDEADBEEF if the pair is broken.
 *
 * @param[in]     pair  pair number
 * @return the value
 */
static uint32_t pair_value(uint32_t pair)
{
    uint32_t v = 0U;

    return stub_backup_get(pair, &v) ? v : 0xDEADBEEFU;
}

/**
 * @brief Returns the RESTARTS value of `count` restarts of `slot` (common/backup_layout.h).
 *
 * @param[in]     slot   BOOT_SLOT_A or BOOT_SLOT_B
 * @param[in]     count  restarts in a row
 * @return the pair value
 */
static uint32_t restarts_of(uint32_t slot, uint32_t count)
{
    return (slot << BACKUP_RESTARTS_SLOT_SHIFT) | count;
}

/**
 * @brief Returns the LAST_START value of a start (common/backup_layout.h).
 *
 * @param[in]     role  BACKUP_ROLE_...
 * @param[in]     slot  slot code
 * @return the pair value
 */
static uint32_t start_of(uint32_t role, uint32_t slot)
{
    return (role << BACKUP_START_ROLE_SHIFT) | slot;
}

/**
 * @brief Returns the FALLBACK value (common/backup_layout.h).
 *
 * @param[in]     refused  refused copy code
 * @param[in]     check    check result code
 * @param[in]     started  started copy code
 * @return the pair value
 */
static uint32_t fallback_of(uint32_t refused, uint32_t check, uint32_t started)
{
    return (started << BACKUP_FALLBACK_STARTED_SHIFT) | (refused << BACKUP_FALLBACK_REFUSED_SHIFT) |
           check;
}

/* ---- The order of the steps ------------------------------------------------------- */

/**
 * @brief The order of the steps of main(): watchdog first; reloads before each write and each
 * candidate; reloads in the park loop.
 *
 * Boot state confirmed A, trial B, 0 failed starts; every image refused. Trace: W C K U (start),
 * R E (count the trial start), R X (trial B refused), R E (trial given up), R X (slot A refused),
 * R X (last-resort refused), O (UART off), R R R (park loop). Unchanged by this change.
 */
static void test_main_order(void)
{
    reset_rig();
    stored.trial = BOOT_SLOT_B;
    region_result[AT_A] = IMAGE_EMPTY;
    region_result[AT_B] = IMAGE_EMPTY;
    region_result[AT_LR] = IMAGE_EMPTY;
    int ended = run_boot();
    report((ended == RUN_PARKED) && (strcmp(ev_trace, "WCKURERXRERXRXORRR") == 0),
           "main: W first, then C K U; R before each E and each X; park loop R R R");
    report(ev_trace[0] == 'W',
           "main: the watchdog is started before the cycle counter, the clock and the UART");
    report((strstr(uart_log, "no firmware can run") != NULL) && !stub_backup_open,
           "main: all images refused -> \"no firmware can run\", backup registers closed, park");
}

/**
 * @brief A cycle counter that does not count is logged, and the bootloader carries on.
 */
static void test_cycles_not_counting(void)
{
    reset_rig();
    stub_cycles_counts = false;
    int ended = run_boot();
    report((ended == RUN_STARTED) && (strstr(uart_log, "cycle counter not counting") != NULL),
           "cycles_start() false -> logged \"cycle counter not counting\"; boot carries on");
}

/**
 * @brief count_trial_start compares before it increments (audit finding #21).
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
    write_results[0] = false;
    state.trial = BOOT_SLOT_B;
    state.failed_starts = 0U;
    count_trial_start(&state);
    report((state.trial == BOOT_SLOT_NONE) &&
               (strstr(uart_log, "could not be counted") != NULL),
           "count_trial_start: the save fails -> trial not started, confirmed slot runs");
}

/* ---- Decision B ------------------------------------------------------------------- */

/**
 * @brief B: watchdog restarts of the confirmed slot are counted; at 3 it is skipped.
 *
 * Fails on the old code: no count existed (the bootloader read the reset cause only for the log),
 * so the confirmed slot was started on every restart for ever.
 */
static void test_b_watchdog_restarts(void)
{
    reset_rig();
    fake_rcc.CSR = RCC_CSR_IWDGRSTF;
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A));
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_A, 1U));
    int ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == restarts_of(BOOT_SLOT_A, 2U)) &&
               (pair_value(BACKUP_PAIR_LAST_START) ==
                start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A)) &&
               (strstr(uart_log, "restarted by the watchdog: 2 in a row") != NULL),
           "B: watchdog restart of confirmed A (1 before) -> 2 in a row, A started, start "
           "recorded");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_IWDGRSTF;
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A));
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_A, 2U));
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_LR) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == restarts_of(BOOT_SLOT_A, 3U)) &&
               (strstr(uart_log, "confirmed slot A skipped") != NULL) &&
               (strstr(uart_log, "slot A accepted") == NULL) &&
               (pair_value(BACKUP_PAIR_LAST_START) ==
                start_of(BACKUP_ROLE_LAST_RESORT, BOOT_SLOT_NONE)),
           "B: third watchdog restart of confirmed A -> A skipped (not checked), last-resort "
           "started");

    reset_rig();
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_A, 3U));
    stored.trial = BOOT_SLOT_B;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_B) && (write_count == 1U),
           "B: confirmed A at 3 restarts, trial B set -> the trial still comes first and starts");
}

/**
 * @brief B: a software restart counts only with the firmware's fault mark; trial starts and
 * other slots do not add to the count.
 *
 * Fails on the old code: nothing was counted.
 */
static void test_b_which_restarts_count(void)
{
    reset_rig();
    fake_rcc.CSR = RCC_CSR_SFTRSTF;
    stub_backup_set(BACKUP_PAIR_FAULT, BACKUP_FAULT_IMPOSSIBLE_VALUE);
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A));
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_A, 2U));
    int ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_LR) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == restarts_of(BOOT_SLOT_A, 3U)) &&
               (pair_value(BACKUP_PAIR_FAULT) == 0U) &&
               (strstr(uart_log, "restarted by its fault reaction: 3 in a row") != NULL),
           "B: software restart marked as a fault reaction -> counted (3), mark cleared, "
           "last-resort started");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_SFTRSTF;
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A));
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_A, 2U));
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == restarts_of(BOOT_SLOT_A, 2U)),
           "B: software restart without the mark (SWITCH, give-up) -> not counted, A started");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_IWDGRSTF;
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_TRIAL, BOOT_SLOT_B));
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_A, 2U));
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == restarts_of(BOOT_SLOT_A, 2U)),
           "B: watchdog restart of a trial start -> not counted here (REQ-015 counts it)");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_IWDGRSTF;
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A));
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_B, 2U));
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == restarts_of(BOOT_SLOT_A, 1U)),
           "B: record of slot B, watchdog restart of confirmed A -> count starts again: A, 1");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_PINRSTF;
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A));
    stub_backup_set(BACKUP_PAIR_RESTARTS, restarts_of(BOOT_SLOT_A, 2U));
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == restarts_of(BOOT_SLOT_A, 2U)),
           "B: reset-pin reset -> not counted, A started");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_IWDGRSTF;
    stub_backup[2U * BACKUP_PAIR_LAST_START] = start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A);
    stub_backup[(2U * BACKUP_PAIR_LAST_START) + 1U] = 0x12345678U;   /* not its complement */
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) && (fault_calls == 0U) &&
               (pair_value(BACKUP_PAIR_RESTARTS) == 0xDEADBEEFU),
           "B: start record with a broken pair (one bit error) -> read as empty: not counted, "
           "no fault");
}

/**
 * @brief B: a restart while the bootloader itself runs is never charged to a slot, and a backup
 * register that cannot be written is logged.
 *
 * Fails on the old code: the start record did not exist.
 */
static void test_b_bootloader_restarts(void)
{
    reset_rig();
    stub_backup_set(BACKUP_PAIR_LAST_START, start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A));
    region_result[AT_A] = IMAGE_EMPTY;
    region_result[AT_LR] = IMAGE_EMPTY;
    int ended = run_boot();
    report((ended == RUN_PARKED) && (pair_value(BACKUP_PAIR_LAST_START) == 0U),
           "B: the start record is cleared at every start and written only at the hand-over (a "
           "parked or hung bootloader leaves it empty)");

    reset_rig();
    stub_backup_writable = false;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) &&
               (strstr(uart_log, "backup registers not writable") != NULL),
           "B: backup registers not writable -> logged, the start goes on");
}

/* ---- Decision D ------------------------------------------------------------------- */

/**
 * @brief D: blank -> default state saved, slot A; damaged -> last-resort only; a corrupted case
 * value -> fault.
 *
 * Fails on the old code: a damaged boot state got the default state (slot A started) as a blank
 * one did.
 */
static void test_d_boot_state(void)
{
    reset_rig();
    stored_load = (int)BOOT_STATE_BLANK;
    int ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) && (write_count == 1U) &&
               (written[0].confirmed == BOOT_SLOT_A) && (written[0].trial == BOOT_SLOT_NONE) &&
               (strstr(uart_log, "boot state blank") != NULL),
           "D: both copies erased -> default (confirmed A, no trial) saved, slot A started");

    reset_rig();
    stored_load = (int)BOOT_STATE_DAMAGED;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_LR) && (write_count == 0U) &&
               (strcmp(ev_trace, "WCKURXH") == 0) &&
               (strstr(uart_log, "boot state damaged") != NULL),
           "D: damaged boot state -> nothing written, only the last-resort checked and started");

    reset_rig();
    stored_load = 7;
    ended = run_boot();
    report((ended == RUN_FAULT) && (strcmp(fault_where, "load_state") == 0) &&
               (started_vector == 0U) && (write_count == 0U),
           "load_state: a case value outside boot_state_load_t (7) -> fault, nothing started");
}

/* ---- Decision F(b) ---------------------------------------------------------------- */

/**
 * @brief F(b): no trial start without a running watchdog; the start is not counted.
 *
 * Fails on the old code: the trial was counted and started whatever the watchdog's state.
 */
static void test_fb_watchdog(void)
{
    reset_rig();
    stored.trial = BOOT_SLOT_B;
    stub_watchdog_running = false;
    int ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) && (write_count == 0U) &&
               (strstr(uart_log, "trial slot B not started: watchdog not confirmed running") !=
                NULL) &&
               (pair_value(BACKUP_PAIR_LAST_START) ==
                start_of(BACKUP_ROLE_CONFIRMED, BOOT_SLOT_A)),
           "F(b): trial B, watchdog not confirmed running -> not counted, not started; "
           "confirmed A started, reason logged");

    reset_rig();
    stored.trial = BOOT_SLOT_B;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_B) && (write_count == 1U) &&
               (written[0].failed_starts == 1U) &&
               (pair_value(BACKUP_PAIR_LAST_START) == start_of(BACKUP_ROLE_TRIAL, BOOT_SLOT_B)),
           "F(b): trial B, watchdog running -> counted (1) and started as the trial");
}

/* ---- Decision G ------------------------------------------------------------------- */

/**
 * @brief G: the firmware's give-up request is applied before the start is counted.
 *
 * Fails on the old code: the request was not read; the trial was counted and started again.
 */
static void test_g_give_up_request(void)
{
    reset_rig();
    stored.trial = BOOT_SLOT_B;
    stored.failed_starts = 1U;
    stub_backup_set(BACKUP_PAIR_GIVE_UP, BOOT_SLOT_B);
    int ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) && (write_count == 1U) &&
               (written[0].trial == BOOT_SLOT_NONE) && (written[0].failed_starts == 0U) &&
               (pair_value(BACKUP_PAIR_GIVE_UP) == 0U) &&
               (strstr(uart_log, "given up: the firmware's give-up request") != NULL),
           "G: request for trial B -> given up and saved before any count, request cleared, A "
           "started");

    reset_rig();
    stored.trial = BOOT_SLOT_B;
    stub_backup_set(BACKUP_PAIR_GIVE_UP, BOOT_SLOT_A);
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_B) && (write_count == 1U) &&
               (written[0].failed_starts == 1U) && (pair_value(BACKUP_PAIR_GIVE_UP) == 0U) &&
               (strstr(uart_log, "names no trial: dropped") != NULL),
           "G: request naming a slot that is not the trial -> dropped and cleared; trial counted, "
           "started");

    reset_rig();
    stored.trial = BOOT_SLOT_B;
    stub_backup_set(BACKUP_PAIR_GIVE_UP, BOOT_SLOT_B);
    write_results[0] = false;
    write_results[1] = false;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) && (write_count == 2U) &&
               (pair_value(BACKUP_PAIR_GIVE_UP) == BOOT_SLOT_B) &&
               (strstr(uart_log, "give-up request kept") != NULL),
           "G: request whose give-up cannot be saved (twice) -> kept for the next start; trial "
           "not started");
}

/* ---- Decision I ------------------------------------------------------------------- */

/**
 * @brief I: the default-state and the give-up saves are tried twice and the result is logged.
 *
 * Fails on the old code: one try, result not checked ((void)save_state).
 */
static void test_i_retry(void)
{
    reset_rig();
    stored_load = (int)BOOT_STATE_BLANK;
    write_results[0] = false;
    int ended = run_boot();
    report((ended == RUN_STARTED) && (write_count == 2U) &&
               (strstr(uart_log, "trying once more") != NULL) &&
               (strstr(uart_log, "written on the second try") != NULL),
           "I: default state, first write fails -> second write, \"written on the second try\"");

    reset_rig();
    stored.trial = BOOT_SLOT_B;
    region_result[AT_B] = IMAGE_EMPTY;
    write_results[1] = false;
    write_results[2] = false;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) && (write_count == 3U) &&
               (strstr(uart_log, "write failed again") != NULL),
           "I: trial image refused, the give-up's two writes fail -> logged, A still started");
}

/* ---- Check-result reporting -------------------------------------------------------- */

/**
 * @brief The copy refused just before the one started, its check result and the started copy are
 * left in the FALLBACK pair; a start without a refusal leaves it empty.
 *
 * Fails on the old code: nothing was left for the firmware.
 */
static void test_fallback_record(void)
{
    reset_rig();
    stored.trial = BOOT_SLOT_B;
    region_result[AT_B] = IMAGE_BAD_SIGNATURE_A;
    int ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_A) &&
               (pair_value(BACKUP_PAIR_FALLBACK) ==
                fallback_of(BACKUP_COPY_B, 0x09U, BACKUP_COPY_A)) &&
               !stub_backup_open,
           "record: trial B refused (signature A), A started -> refused 0x02, check 0x09, "
           "started 0x01; backup registers closed before the jump");

    reset_rig();
    stub_backup_set(BACKUP_PAIR_FALLBACK,
                    fallback_of(BACKUP_COPY_A, 0x08U, BACKUP_COPY_LAST_RESORT));
    ended = run_boot();
    report((ended == RUN_STARTED) && (pair_value(BACKUP_PAIR_FALLBACK) == 0U),
           "record: nothing refused -> an old record is cleared, so it is never reported later");

    reset_rig();
    region_result[AT_A] = IMAGE_BAD_BODY_CHECK;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_LR) &&
               (pair_value(BACKUP_PAIR_FALLBACK) ==
                fallback_of(BACKUP_COPY_A, 0x08U, BACKUP_COPY_LAST_RESORT)),
           "record: confirmed A refused (body check value), last-resort started -> 0x01, 0x08, "
           "0x03");

    reset_rig();
    stored.trial = BOOT_SLOT_B;
    region_result[AT_B] = IMAGE_EMPTY;
    region_result[AT_A] = IMAGE_BAD_START;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_LR) &&
               (pair_value(BACKUP_PAIR_FALLBACK) ==
                fallback_of(BACKUP_COPY_A, 0x07U, BACKUP_COPY_LAST_RESORT)),
           "record: trial B and confirmed A refused -> the record names A (the copy refused just "
           "before the one started)");

    reset_rig();
    stub_ecc_address = SLOT_A_START;
    ended = run_boot();
    report((ended == RUN_STARTED) && (started_vector == VECTOR_LR) &&
               (pair_value(BACKUP_PAIR_FALLBACK) == 0U) &&
               (strstr(uart_log, "no check-result code for a flash read error") != NULL),
           "record: A refused only for a flash read error -> no table code, no record, logged");
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
    test_b_watchdog_restarts();
    test_b_which_restarts_count();
    test_b_bootloader_restarts();
    test_d_boot_state();
    test_fb_watchdog();
    test_g_give_up_request();
    test_i_retry();
    test_fallback_record();
    if (failures == 0U) {
        printf("bootloader flow tests: all %u passed\n", tests);
        return 0;
    }
    printf("bootloader flow tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
