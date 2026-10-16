/*
 * fz_backup.c — fuzz target 5: the fault-location record and the other words the bootloader
 * keeps in the TAMP backup registers [common/backup_layout.h], read through the bootloader's
 * real read path: the real bootloader/backup.c against RAM registers (the layout of
 * test/controller/unit/backup_stubs, in stubs/stm32g4xx.h), and the bootloader's own main.c,
 * included as test_boot.c includes it, so its static functions are called as they run on the
 * board.
 *
 * Part A, per input, one of the functions that read the registers, on fuzzed register values:
 *   - count_restart [main.c:640]: LAST_START, FAULT, RESTARTS;
 *   - confirmed_skipped [main.c:699]: RESTARTS;
 *   - apply_give_up_request [main.c:540]: GIVE_UP, with the boot state in a RAM flash;
 *   - backup_leave_fault_location [backup.c]: FAULT_LOCATION and FAULT_LATER, the register
 *     words that the firmware reads back as EVENT 0x0005.
 * The registers get matching pairs (value and complement) with every kind of value, broken pairs
 * (one bit of the complement flipped, both words 0, random words) and random free registers.
 * Each function is compared with a model written from the comments of backup_layout.h and
 * main.c; the model checks the registers after the call. The fault reaction is recorded.
 *
 * Part B (every 100th input): the whole bootloader_main with fuzzed registers, boot-state copies,
 * reset cause and slot contents (the build's signed images). Oracles: an image that is started
 * passes the firmware's image_check (the other implementation); a damaged boot state starts only
 * the last-resort firmware (decision D); a trial is started only with a start count below the
 * limit; a confirmed slot with 3 counted restarts is not started as the confirmed slot
 * (decision B).
 *
 * What is reported and not decided: a pair that matches its complement but holds a value that
 * cannot exist (a slot code 0x77 in GIVE_UP, bits above bit 15 in RESTARTS) is not a fault in this
 * code; backup_layout.h and fault.h say a matching pair is the check, and only a broken pair
 * reads as empty. CODE_RULES B1 and the brief of this audit say a value that passes its check but
 * cannot exist is corruption and reaches the fault reaction. The log lists, per pair, how many
 * such values were seen and what the code did.
 */
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "fz_common.h"
#include "fz_fault.h"
#include "fz_ramflash.h"
#include "image_check.h"

#define main bootloader_main
#include "main.c"
#undef main

/* ---- Constants -------------------------------------------------------------------------- */

RCC_TypeDef fake_rcc;
PWR_TypeDef fake_pwr;
TAMP_TypeDef fake_tamp;

#define PART_B_EVERY      100U
#define REGISTERS         BACKUP_REGISTERS
#define FUNCTIONS         4U
#define RUN_PARKED        1
#define RUN_STARTED       2
#define SLOT_ERASED_BYTE  0xFFU
/* Values that make a matching pair "impossible" are counted per pair for the log table. */
#define PAIR_NAMES        BACKUP_PAIRS

static const char *const PAIR_NAME[PAIR_NAMES] = {"RESTARTS", "LAST_START", "GIVE_UP", "FAULT",
                                                  "FALLBACK", "FAULT_LOCATION", "FAULT_LATER"};

static jmp_buf g_run_exit;
static volatile bool g_run_armed;
static volatile int g_run_how;
static volatile uint32_t g_started_vector;
static uint32_t g_reloads_after_off;
static bool g_uart_off;
static bool g_watchdog_running = true;
static uint64_t g_impossible_seen[PAIR_NAMES];
static uint64_t g_impossible_fault[PAIR_NAMES];
static uint64_t g_pair_matching_seen[PAIR_NAMES];
static uint64_t g_part_b_runs;
static uint64_t g_part_b_started;
static uint64_t g_part_b_parked;
static uint64_t g_function_runs[FUNCTIONS];
static uint8_t g_images[4][SLOT_BYTES];
static uint32_t g_image_sizes[4];
static fz_guarded_t g_guard;

/* ---- Stand-ins for the chip around main.c ----------------------------------------------- */

/**
 * @brief Host stand-in for uart_init.
 */
void uart_init(void)
{
}

/**
 * @brief Host stand-in for uart_put: the text is dropped.
 *
 * @param[in]     text  text
 */
void uart_put(const char *text)
{
    (void)text;
}

/**
 * @brief Host stand-in for uart_put_decimal: dropped.
 *
 * @param[in]     value  value
 */
void uart_put_decimal(uint32_t value)
{
    (void)value;
}

/**
 * @brief Host stand-in for uart_put_hex32: dropped.
 *
 * @param[in]     value  value
 */
void uart_put_hex32(uint32_t value)
{
    (void)value;
}

/**
 * @brief Host stand-in for uart_wait_sent.
 */
void uart_wait_sent(void)
{
}

/**
 * @brief Host stand-in for uart_off: from now on the park loop is detected by reloads.
 */
void uart_off(void)
{
    g_uart_off = true;
}

/**
 * @brief Host stand-in for watchdog_start_long.
 */
void watchdog_start_long(void)
{
}

/**
 * @brief Host stand-in for watchdog_reload: the third reload after uart_off ends the run as the
 * park loop (the same rule as test/controller/unit/boot_stubs/boot_stubs.c).
 */
void watchdog_reload(void)
{
    if (g_uart_off) {
        g_reloads_after_off++;
        if ((g_reloads_after_off >= 3U) && g_run_armed) {
            g_run_armed = false;
            g_run_how = RUN_PARKED;
            longjmp(g_run_exit, RUN_PARKED);
        }
    }
}

/**
 * @brief Host stand-in for watchdog_set_firmware_timeout.
 */
void watchdog_set_firmware_timeout(void)
{
}

/**
 * @brief Host stand-in for watchdog_running.
 *
 * @param[out]    iwdg_sr    register value read
 * @param[out]    rcc_csr    register value read
 * @param[out]    waited_us  time waited
 * @return the value of g_watchdog_running
 */
bool watchdog_running(uint32_t *iwdg_sr, uint32_t *rcc_csr, uint32_t *waited_us)
{
    *iwdg_sr = 0U;
    *rcc_csr = 0U;
    *waited_us = 0U;
    return g_watchdog_running;
}

/**
 * @brief Host stand-in for cycles_start.
 *
 * @return true
 */
bool cycles_start(void)
{
    return true;
}

/**
 * @brief Host stand-in for cycles_now.
 *
 * @return 0
 */
uint32_t cycles_now(void)
{
    return 0U;
}

/**
 * @brief Host stand-in for clock_to_170mhz.
 *
 * @return true
 */
bool clock_to_170mhz(void)
{
    return true;
}

/**
 * @brief Host stand-in for clock_hz.
 *
 * @return the core clock [clock.h]
 */
uint32_t clock_hz(void)
{
    return CORE_CLOCK_HZ;
}

/**
 * @brief Host stand-in for clock_back_to_reset.
 */
void clock_back_to_reset(void)
{
}

/**
 * @brief Host stand-in for handover_start: ends the run, remembering the vector table address.
 *
 * @param[in]     vector_table  where the started firmware's vector table is
 * @note Does not return.
 */
void handover_start(uint32_t vector_table)
{
    g_started_vector = vector_table;
    if (g_run_armed) {
        g_run_armed = false;
        g_run_how = RUN_STARTED;
        longjmp(g_run_exit, RUN_STARTED);
    }
    fprintf(stderr, "handover_start outside a guarded run\n");
    abort();
}

/* ---- Helpers ---------------------------------------------------------------------------- */

/**
 * @brief Stores a matching pair in the RAM registers.
 *
 * @param[in]     pair   pair number
 * @param[in]     value  the value
 */
static void set_pair(uint32_t pair, uint32_t value)
{
    fake_tamp.BKP[2U * pair] = value;
    fake_tamp.BKP[(2U * pair) + 1U] = ~value;
}

/**
 * @brief Reads a pair the way backup.c does.
 *
 * @param[in]     pair   pair number
 * @param[out]    value  the value, 0 if the pair is broken
 * @return true if the pair matches
 */
static bool get_pair(uint32_t pair, uint32_t *value)
{
    uint32_t a = fake_tamp.BKP[2U * pair];
    uint32_t b = fake_tamp.BKP[(2U * pair) + 1U];

    *value = (a == ~b) ? a : 0U;
    return a == ~b;
}

/**
 * @brief Returns true if a matching pair holds a value that cannot exist for that pair
 * (backup_layout.h: the slot codes, roles, reasons, check codes and locations it lists).
 *
 * @param[in]     pair   pair number
 * @param[in]     value  the value of the matching pair
 * @return true if the value is outside what the layout lists
 */
static bool value_impossible(uint32_t pair, uint32_t value)
{
    uint32_t b0 = value & 0xFFU;
    uint32_t b1 = (value >> 8) & 0xFFU;
    uint32_t b2 = (value >> 16) & 0xFFU;
    uint32_t b3 = (value >> 24) & 0xFFU;

    switch (pair) {
    case BACKUP_PAIR_RESTARTS:
        return ((value >> 16) != 0U) || ((b1 != BOOT_SLOT_A) && (b1 != BOOT_SLOT_B)
                                         && (value != 0U));
    case BACKUP_PAIR_LAST_START:
        return (value != 0U) && (((value >> 16) != 0U) || (b0 > BOOT_SLOT_B) ||
                                 ((b1 != BACKUP_ROLE_CONFIRMED) && (b1 != BACKUP_ROLE_TRIAL)
                                  && (b1 != BACKUP_ROLE_LAST_RESORT)));
    case BACKUP_PAIR_GIVE_UP:
        return value > BOOT_SLOT_B;
    case BACKUP_PAIR_FAULT:
        return value > BACKUP_FAULT_SELF_TEST;
    case BACKUP_PAIR_FALLBACK:
        return (value != 0U) && ((b0 > CHECK_LAST) || (b1 > CHECK_LAST) || (b2 > CHECK_LAST)
                                 || (b3 > BACKUP_COPY_LAST_RESORT));
    case BACKUP_PAIR_FAULT_LOCATION:
        return value > FAULT_RANGE_COMMON_LAST;
    case BACKUP_PAIR_FAULT_LATER:
        return value > BACKUP_FAULT_LATER_MAX;
    }
    return false;
}

/**
 * @brief Fills the registers: for every pair a matching, broken or zero pair; the two free
 * registers random.
 *
 * @param[in,out] rng  the generator
 */
static void fill_registers(fz_rng_t *rng)
{
    for (uint32_t pair = 0U; pair < BACKUP_PAIRS; pair++) {
        uint32_t how = fz_rng_below(rng, 10U);
        uint32_t value = fz_rng_chance(rng, 500U) ? fz_rng_edge32(rng) : (uint32_t)fz_rng_next(rng);

        if (fz_rng_chance(rng, 500U)) {   /* a value the layout lists */
            switch (pair) {
            case BACKUP_PAIR_RESTARTS:
                value = ((uint32_t)(1U + fz_rng_below(rng, 2U)) << BACKUP_RESTARTS_SLOT_SHIFT)
                    | fz_rng_below(rng, 6U);
                break;
            case BACKUP_PAIR_LAST_START:
                value = ((1U + fz_rng_below(rng, 3U)) << BACKUP_START_ROLE_SHIFT)
                    | fz_rng_below(rng, 3U);
                break;
            case BACKUP_PAIR_GIVE_UP:
            case BACKUP_PAIR_FAULT:
                value = fz_rng_below(rng, 3U);
                break;
            case BACKUP_PAIR_FALLBACK:
                value = (fz_rng_below(rng, CHECK_LAST + 1U))
                    | (fz_rng_below(rng, CHECK_LAST + 1U) << 8) |
                        (fz_rng_below(rng, CHECK_LAST + 1U) << 16) | (fz_rng_below(rng, 4U) << 24);
                break;
            case BACKUP_PAIR_FAULT_LOCATION:
                value = fz_rng_below(rng, 0x20U);
                break;
            case BACKUP_PAIR_FAULT_LATER:
                value = fz_rng_below(rng, 0x100U);
                break;
            }
        }
        if (how < 6U) {
            set_pair(pair, value);
        } else if (how < 7U) {
            set_pair(pair, value);
            fake_tamp.BKP[(2U * pair) + 1U] ^= (uint32_t)(1ULL << fz_rng_below(rng, 32U));
        } else if (how < 8U) {
            set_pair(pair, value);
            fake_tamp.BKP[2U * pair] ^= (uint32_t)(1ULL << fz_rng_below(rng, 32U));
        } else if (how < 9U) {
            fake_tamp.BKP[2U * pair] = 0U;
            fake_tamp.BKP[(2U * pair) + 1U] = 0U;
        } else {
            fake_tamp.BKP[2U * pair] = (uint32_t)fz_rng_next(rng);
            fake_tamp.BKP[(2U * pair) + 1U] = (uint32_t)fz_rng_next(rng);
        }
    }
    fake_tamp.BKP[2U * BACKUP_PAIRS] = (uint32_t)fz_rng_next(rng);
    fake_tamp.BKP[(2U * BACKUP_PAIRS) + 1U] = (uint32_t)fz_rng_next(rng);
}

/**
 * @brief Counts, per pair, the matching pairs and those with an impossible value.
 */
static void count_pairs(void)
{
    for (uint32_t pair = 0U; pair < BACKUP_PAIRS; pair++) {
        uint32_t value;
        if (get_pair(pair, &value)) {
            g_pair_matching_seen[pair]++;
            if (value_impossible(pair, value)) {
                g_impossible_seen[pair]++;
            }
        }
    }
}

/**
 * @brief Builds a random valid boot state.
 *
 * @param[in,out] rng    the generator
 * @param[out]    state  the state
 */
static void random_state(fz_rng_t *rng, boot_state_t *state)
{
    memset(state, 0, sizeof *state);
    state->confirmed = fz_rng_chance(rng, 500U) ? (uint8_t)BOOT_SLOT_A : (uint8_t)BOOT_SLOT_B;
    state->trial = fz_rng_chance(rng, 400U) ? (uint8_t)BOOT_SLOT_NONE
                                            : (uint8_t)((state->confirmed == BOOT_SLOT_A)
                                                        ? BOOT_SLOT_B : BOOT_SLOT_A);
    state->failed_starts = (uint8_t)fz_rng_below(rng, BOOT_STATE_MAX_STARTS + 1U);
    state->flags = (uint8_t)fz_rng_below(rng, 2U);
    state->given_up_check = fz_rng_chance(rng, 500U) ? 0U : (uint32_t)fz_rng_next(rng);
    state->sequence = fz_rng_below(rng, 100U);
}

/**
 * @brief Fails the input.
 *
 * @param[in]     kind   failure kind
 * @param[in]     index  input index
 * @param[in]     what   what was wrong
 */
static void fail_regs(const char *kind, uint64_t index, const char *what)
{
    fz_fail(kind, index, (const uint8_t *)fake_tamp.BKP, sizeof fake_tamp.BKP, what);
}

/* ---- Part A ----------------------------------------------------------------------------- */

/**
 * @brief Runs count_restart on the registers and compares the registers after with the model.
 *
 * @param[in,out] rng    the generator
 * @param[in]     index  input index
 */
static void check_count_restart(fz_rng_t *rng, uint64_t index)
{
    reset_cause_t cause = (reset_cause_t)fz_rng_below(rng, 5U);
    uint32_t last = 0U;
    uint32_t fault = 0U;
    uint32_t rec = 0U;
    bool last_known = get_pair(BACKUP_PAIR_LAST_START, &last);
    bool fault_marked = get_pair(BACKUP_PAIR_FAULT, &fault) && (fault != 0U);
    bool rec_known = get_pair(BACKUP_PAIR_RESTARTS, &rec);
    uint32_t role = (last >> BACKUP_START_ROLE_SHIFT) & BACKUP_BYTE_MASK;
    uint32_t slot = last & BACKUP_BYTE_MASK;
    bool counted_cause = (cause == RESET_WATCHDOG) || ((cause == RESET_SOFTWARE) && fault_marked);
    bool counts = counted_cause && last_known && (role == BACKUP_ROLE_CONFIRMED) &&
                  ((slot == BOOT_SLOT_A) || (slot == BOOT_SLOT_B));
    uint32_t expect = rec_known ? rec : 0U;
    uint32_t count = 0U;
    bool expect_known = rec_known;
    bool faulted = false;
    uint32_t after;

    if (counts) {
        if (rec_known && (((rec >> BACKUP_RESTARTS_SLOT_SHIFT) & BACKUP_BYTE_MASK) == slot)) {
            count = rec & BACKUP_BYTE_MASK;
        }
        if (count < RESTARTS_COUNT_MAX) {
            count++;
        }
        expect = (slot << BACKUP_RESTARTS_SLOT_SHIFT) | count;
        expect_known = true;
    }
    FZ_FAULT_GUARD(faulted, count_restart(cause));
    if (faulted) {
        g_impossible_fault[BACKUP_PAIR_LAST_START]++;
        fail_regs("fault-from-register", index, "count_restart reached the fault reaction");
        return;
    }
    bool known_after = get_pair(BACKUP_PAIR_RESTARTS, &after);
    if ((known_after != expect_known) || (after != expect)) {
        fail_regs("restart-count", index, "RESTARTS after count_restart differs from the model");
    }
    if (!get_pair(BACKUP_PAIR_LAST_START, &after) || (after != 0U) ||
        !get_pair(BACKUP_PAIR_FAULT, &after) || (after != 0U)) {
        fail_regs("not-cleared", index, "LAST_START or FAULT not cleared by count_restart");
    }
}

/**
 * @brief Runs confirmed_skipped and compares the answer with the model.
 *
 * @param[in,out] rng    the generator
 * @param[in]     index  input index
 */
static void check_confirmed_skipped(fz_rng_t *rng, uint64_t index)
{
    boot_state_t state;
    uint32_t rec = 0U;
    bool known = get_pair(BACKUP_PAIR_RESTARTS, &rec);
    bool faulted = false;
    bool got = false;
    bool expect;

    random_state(rng, &state);
    expect = known
        && (((rec >> BACKUP_RESTARTS_SLOT_SHIFT) & BACKUP_BYTE_MASK) == state.confirmed) &&
             ((rec & BACKUP_BYTE_MASK) >= CONFIRMED_RESTARTS_MAX);
    FZ_FAULT_GUARD(faulted, got = confirmed_skipped(&state));
    if (faulted) {
        g_impossible_fault[BACKUP_PAIR_RESTARTS]++;
        fail_regs("fault-from-register", index, "confirmed_skipped reached the fault reaction");
    } else if (got != expect) {
        fail_regs("skip-decision", index, "confirmed_skipped differs from the model");
    }
}

/**
 * @brief Runs apply_give_up_request with a boot state and compares state and register after.
 *
 * @param[in,out] rng    the generator
 * @param[in]     index  input index
 */
static void check_give_up(fz_rng_t *rng, uint64_t index)
{
    boot_state_t state;
    boot_state_t before;
    uint32_t request = 0U;
    bool known = get_pair(BACKUP_PAIR_GIVE_UP, &request);
    bool faulted = false;
    uint32_t after = 0U;

    random_state(rng, &state);
    before = state;
    ramflash_reset();
    if (fz_rng_chance(rng, 600U)) {   /* a header with the proprietary string in the trial slot */
        uint8_t *h = ramflash_ptr((state.trial == BOOT_SLOT_A) ? SLOT_A_START : SLOT_B_START);
        memcpy(h, "FSIR", 4U);
        h[IMAGE_OFFSET_BODY_CHECK] = 0x78U;
    }
    FZ_FAULT_GUARD(faulted, apply_give_up_request(&state));
    if (faulted) {
        g_impossible_fault[BACKUP_PAIR_GIVE_UP]++;
        fail_regs("fault-from-register", index, "apply_give_up_request reached the fault reaction");
        return;
    }
    bool gave_up = known && (request != 0U) && (before.trial != BOOT_SLOT_NONE)
        && (request == before.trial);
    if (gave_up != ((state.trial == BOOT_SLOT_NONE) && (before.trial != BOOT_SLOT_NONE))) {
        fail_regs("give-up", index, "the trial was given up (or not) against the model");
    }
    if (gave_up && ((state.flags != BOOT_STATE_FLAG_FALLBACK) || (state.failed_starts != 0U))) {
        fail_regs("give-up", index,
                  "a given-up trial left FALLBACK unset or failed starts above 0");
    }
    if (!gave_up && memcmp(&state, &before, sizeof state) != 0) {
        fail_regs("give-up", index, "the boot state changed without a matching give-up request");
    }
    if (known && (request != 0U)) {
        if (!get_pair(BACKUP_PAIR_GIVE_UP, &after) || (after != 0U)) {
            fail_regs("give-up", index,
                      "a request was not cleared after it was applied or dropped");
        }
    }
}

/**
 * @brief Runs backup_leave_fault_location (the fault reaction's write) on the registers and
 * checks the first-fault rule.
 *
 * @param[in,out] rng    the generator
 * @param[in]     index  input index
 */
static void check_leave_location(fz_rng_t *rng, uint64_t index)
{
    uint8_t location = (uint8_t)(1U + fz_rng_below(rng, FAULT_RANGE_COMMON_LAST));
    uint32_t stored = 0U;
    uint32_t later = 0U;
    bool had = get_pair(BACKUP_PAIR_FAULT_LOCATION, &stored) && (stored != 0U);
    bool later_known = get_pair(BACKUP_PAIR_FAULT_LATER, &later);
    uint32_t expect_later = had ? (later_known ? later : 0U) : 0U;
    uint32_t expect_location = had ? stored : location;
    uint32_t got_location = 0U;
    uint32_t got_later = 0U;

    if (had && (expect_later < BACKUP_FAULT_LATER_MAX)) {
        expect_later++;
    }
    backup_leave_fault_location(location);
    bool ok_l = get_pair(BACKUP_PAIR_FAULT_LOCATION, &got_location);
    bool ok_c = get_pair(BACKUP_PAIR_FAULT_LATER, &got_later);
    if (had && ((!ok_l) || (got_location != expect_location))) {
        fail_regs("first-fault", index, "a stored location was overwritten by a later fault");
    }
    if (!had && ((!ok_l) || (got_location != expect_location) || (!ok_c) || (got_later != 0U))) {
        fail_regs("first-fault", index,
                  "the first fault did not store its location with a count of 0");
    }
    if (had && ((!ok_c) || (got_later != expect_later))) {
        fail_regs("later-count", index, "the later-faults count differs from the model");
    }
}

/* ---- Part B ----------------------------------------------------------------------------- */

/**
 * @brief Puts the images of the build into the RAM flash, damaged or erased with some chance.
 *
 * @param[in,out] rng  the generator
 */
static void fill_flash(fz_rng_t *rng)
{
    static const uint32_t starts[3] = {SLOT_A_START, SLOT_B_START, LAST_RESORT_START};
    static const uint32_t bytes[3] = {SLOT_BYTES, SLOT_BYTES, LAST_RESORT_BYTES};

    ramflash_reset();
    for (uint32_t r = 0U; r < 3U; r++) {
        uint32_t pick = r;   /* image 0 slot A, 1 slot B, 2 last-resort */
        uint32_t how = fz_rng_below(rng, 10U);
        uint8_t *dst = ramflash_ptr(starts[r]);

        if (how < 7U) {
            memcpy(dst, g_images[pick], g_image_sizes[pick] < bytes[r] ? g_image_sizes[pick]
                   : bytes[r]);
        } else if (how < 8U) {
            memcpy(dst, g_images[pick], g_image_sizes[pick] < bytes[r] ? g_image_sizes[pick]
                   : bytes[r]);
            dst[fz_rng_below(rng, g_image_sizes[pick])] ^= (uint8_t)(1U << fz_rng_below(rng, 8U));
        } else if (how < 9U) {
            memcpy(dst, g_images[(pick + 1U) % 3U], g_image_sizes[(pick + 1U) % 3U] < bytes[r]
                   ? g_image_sizes[(pick + 1U) % 3U] : bytes[r]);
        }   /* otherwise: erased */
    }
}

/**
 * @brief Puts boot-state copies in the RAM flash: valid, blank, damaged or one of each.
 *
 * @param[in,out] rng    the generator
 * @param[out]    state  the state the bootloader should find, if the copies hold one
 * @param[out]    valid  true if a valid copy was stored
 * @param[out]    blank  true if both copies were left blank
 */
static void fill_boot_state(fz_rng_t *rng, boot_state_t *state, bool *valid, bool *blank)
{
    uint32_t how = fz_rng_below(rng, 10U);

    *valid = false;
    *blank = false;
    random_state(rng, state);
    if (how < 6U) {
        boot_state_t w = *state;
        *valid = bl_boot_state_write(&w);
        *state = w;
    } else if (how < 8U) {
        *blank = true;
    } else {
        memset(ramflash_ptr(BOOT_STATE_COPY_0), 0x5A, 16U);   /* written, not valid, not blank */
    }
}

/**
 * @brief Runs the whole bootloader_main on fuzzed registers, boot state, images and reset cause.
 *
 * @param[in,out] rng    the generator
 * @param[in]     index  input index
 */
static void check_whole_bootloader(fz_rng_t *rng, uint64_t index)
{
    boot_state_t state;
    bool valid;
    bool blank;
    bool faulted = false;
    int how = 0;
    uint32_t rec = 0U;
    bool rec_known;
    static const uint32_t causes[5] = {RCC_CSR_IWDGRSTF, RCC_CSR_BORRSTF, RCC_CSR_SFTRSTF,
                                       RCC_CSR_PINRSTF, 0U};

    fill_flash(rng);
    fill_boot_state(rng, &state, &valid, &blank);
    fill_registers(rng);
    fake_rcc.CSR = causes[fz_rng_below(rng, 5U)];
    g_watchdog_running = !fz_rng_chance(rng, 100U);
    rec_known = get_pair(BACKUP_PAIR_RESTARTS, &rec);
    g_uart_off = false;
    g_reloads_after_off = 0U;
    g_started_vector = 0U;
    fz_fault_clear();
    g_part_b_runs++;
    fz_fault_armed = true;
    g_run_armed = true;
    if (setjmp(fz_fault_exit) == 0) {
        if (setjmp(g_run_exit) == 0) {
            bootloader_main();
        }
        how = g_run_how;
        fz_fault_armed = false;
    } else {
        faulted = true;
    }
    g_run_armed = false;
    fz_fault_armed = false;
    if (faulted) {
        fail_regs("fault-from-register", index, "the whole bootloader reached the fault reaction");
        return;
    }
    if (how == RUN_PARKED) {
        g_part_b_parked++;
        return;
    }
    g_part_b_started++;
    const uint32_t starts[3] = {SLOT_A_START, SLOT_B_START, LAST_RESORT_START};
    const uint32_t bytes[3] = {SLOT_BYTES, SLOT_BYTES, LAST_RESORT_BYTES};
    const uint8_t kinds[3] = {IMAGE_KIND_MAIN, IMAGE_KIND_MAIN, IMAGE_KIND_LAST_RESORT};
    int region = -1;

    for (int r = 0; r < 3; r++) {
        if (g_started_vector == (starts[r] + (uint32_t)IMAGE_CODE_OFFSET)) {
            region = r;
        }
    }
    if (region < 0) {
        fail_regs("started-vector", index,
                  "handover to an address that is no region's vector table");
        return;
    }
    uint32_t signed_bytes = 0U;
    const uint8_t *image = ramflash_ptr(starts[region]);
    if ((image_check_contents(image, starts[region], bytes[region], kinds[region],
                              &signed_bytes) != IMAGE_OK) ||
        (image_check_signatures(image, signed_bytes) != IMAGE_OK)) {
        fail_regs("started-bad-image", index,
                  "the bootloader started an image that the firmware's check refuses");
    }
    if (!valid && !blank && (region != 2)) {
        fail_regs("started-damaged-state", index,
                  "slot A or B started with a damaged boot state (decision D)");
    }
    if (valid && (region != 2) && (state.trial != BOOT_SLOT_NONE)
        && (state.failed_starts >= BOOT_STATE_MAX_STARTS) &&
        (g_started_vector == (starts[(state.trial == BOOT_SLOT_A) ? 0
                                     : 1] + (uint32_t)IMAGE_CODE_OFFSET))) {
        fail_regs("trial-over-limit", index,
                  "a trial with the maximum start count was started again");
    }
    {
        uint32_t last = 0U;
        bool last_known = get_pair(BACKUP_PAIR_LAST_START, &last);
        uint32_t role = (last >> BACKUP_START_ROLE_SHIFT) & BACKUP_BYTE_MASK;
        uint32_t slot = last & BACKUP_BYTE_MASK;
        uint32_t expect_slot = (region == 0) ? BOOT_SLOT_A
            : ((region == 1) ? BOOT_SLOT_B : BOOT_SLOT_NONE);

        if (!last_known || (slot != expect_slot) || (role == 0U)
            || (role > BACKUP_ROLE_LAST_RESORT)) {
            fail_regs("last-start", index, "LAST_START not stored for the started copy");
        }
    }
    (void)rec_known;
    (void)rec;
}

/**
 * @brief Runs one input.
 *
 * @param[in]     seed   run seed
 * @param[in]     index  input index
 */
static void run_input(uint64_t seed, uint64_t index)
{
    fz_rng_t rng;
    uint32_t which;

    fz_rng_seed(&rng, seed, index);
    fz_input_begin(index, (const uint8_t *)fake_tamp.BKP, 0U);
    fz_fault_clear();
    memset(&fake_tamp, 0, sizeof fake_tamp);
    fake_pwr.CR1 = 0U;
    fake_rcc.APB1ENR1 = 0U;
    if ((index % PART_B_EVERY) == (PART_B_EVERY - 1U)) {
        check_whole_bootloader(&rng, index);
    } else {
        fill_registers(&rng);
        count_pairs();
        which = fz_rng_below(&rng, FUNCTIONS);
        g_function_runs[which]++;
        backup_open();
        ramflash_reset();
        if (which == 0U) {
            check_count_restart(&rng, index);
        } else if (which == 1U) {
            check_confirmed_skipped(&rng, index);
        } else if (which == 2U) {
            check_give_up(&rng, index);
        } else {
            check_leave_location(&rng, index);
        }
    }
    fz_input_end();
}

/**
 * @brief Loads the signed images the whole-bootloader run starts.
 *
 * @param[in]     options  run options (--dir paths)
 * @return true if all three were found
 */
static bool load_images(const fz_options_t *options)
{
    static const char *const names[3] = {"firmware-A.img", "firmware-B.img", "last-resort-v1.img"};

    for (uint32_t i = 0U; i < 3U; i++) {
        for (uint32_t d = 0U; d < options->dir_count; d++) {
            char path[512];
            (void)snprintf(path, sizeof path, "%s/%s", options->dirs[d], names[i]);
            FILE *file = fopen(path, "rb");
            if (file != NULL) {
                g_image_sizes[i] = (uint32_t)fread(g_images[i], 1U, SLOT_BYTES, file);
                (void)fclose(file);
            }
        }
        if (g_image_sizes[i] == 0U) {
            fz_log("missing image %s in the --dir directories", names[i]);
            return false;
        }
    }
    return true;
}

/**
 * @brief Runs the backup-register fuzz target.
 *
 * @param[in]     argc  argument count
 * @param[in]     argv  arguments: see fz_options_parse
 * @return 0 if no input failed, 1 otherwise
 */
int main(int argc, char **argv)
{
    fz_options_t options;
    uint64_t first = 0U;
    uint64_t last;

    fz_options_parse(&options, argc, argv);
    fz_run_open("fz_backup", &options, argc, argv);
    fz_guarded_init(&g_guard, 4096U);
    if (!load_images(&options)) {
        return 2;
    }
    last = options.count;
    if (options.one) {
        first = options.one_index;
        last = first + 1U;
    }
    for (uint64_t index = first; index < last; index++) {
        run_input(options.seed, index);
    }
    fz_log("part A function runs: count_restart %llu, confirmed_skipped %llu, "
           "apply_give_up_request %llu, "
           "backup_leave_fault_location %llu; part B (whole bootloader_main) runs: %llu (started "
               "%llu, parked %llu)",
           (unsigned long long)g_function_runs[0], (unsigned long long)g_function_runs[1],
           (unsigned long long)g_function_runs[2], (unsigned long long)g_function_runs[3],
           (unsigned long long)g_part_b_runs, (unsigned long long)g_part_b_started,
           (unsigned long long)g_part_b_parked);
    fz_log("matching pairs with a value that cannot exist (backup_layout.h), per pair: seen / "
           "matching pairs seen / reached the fault reaction:");
    for (uint32_t pair = 0U; pair < PAIR_NAMES; pair++) {
        fz_log("  %-15s %llu / %llu / %llu", PAIR_NAME[pair],
               (unsigned long long)g_impossible_seen[pair],
               (unsigned long long)g_pair_matching_seen[pair],
                   (unsigned long long)g_impossible_fault[pair]);
    }
    return fz_run_finish(last - first);
}
