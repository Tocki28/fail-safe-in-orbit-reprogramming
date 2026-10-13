/*
 * main.c — the bootloader (Architecture Sec. 5.1, Figure 3): choose the firmware to run, check
 * it, start it, and fall back when it cannot run.
 *
 * Order of candidates: the trial slot if a trial is set, then the confirmed slot, then the
 * last-resort firmware (Architecture Sec. 5.1, Figure 3: trial, previous copy, last-resort).
 * A slot that is neither on trial nor confirmed is never started: it may hold a trial that was
 * given up, and failed firmware must not start again unless the operator orders it (REQ-019).
 * This is MCUboot's run-in-place mode with test and revert (MCUboot design.md, Sec. "Equal
 * slots (direct-xip)" and "Revert mechanism in direct-xip and ram-load mode"), plus the
 * last-resort firmware of Architecture Sec. 5.1.
 *
 * Every line on USART2 starts with "BL: ".
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot_state.h"
#include "clock.h"
#include "flash.h"
#include "handover.h"
#include "image_check.h"
#include "image_format.h"
#include "layout.h"
#include "stm32g4xx.h"
#include "uart.h"
#include "watchdog.h"

/* Trial slot, confirmed slot, last-resort. */
#define MAX_CANDIDATES  3U

typedef struct {
    const char *name;   /* for the log */
    uint32_t start;     /* first byte of the region */
    uint32_t bytes;     /* size of the region */
    uint8_t kind;       /* the only image kind this region may run */
    uint8_t slot;       /* BOOT_SLOT_A/B, or BOOT_SLOT_NONE for the last-resort region */
} region_t;

static const region_t REGION_A = {"slot A", SLOT_A_START, SLOT_BYTES, IMAGE_KIND_MAIN, BOOT_SLOT_A};
static const region_t REGION_B = {"slot B", SLOT_B_START, SLOT_BYTES, IMAGE_KIND_MAIN, BOOT_SLOT_B};
static const region_t REGION_LAST_RESORT = {"last-resort", LAST_RESORT_START, LAST_RESORT_BYTES,
                                            IMAGE_KIND_LAST_RESORT, BOOT_SLOT_NONE};

/* ---- Log lines -------------------------------------------------------------------- */

/**
 * @brief Writes one log line to USART2.
 *
 * The line is "BL: ", the three strings one after the other, and a line end.
 *
 * @param[in]     a  first string
 * @param[in]     b  second string
 * @param[in]     c  third string
 */
static void log_text(const char *a, const char *b, const char *c)
{
    uart_put("BL: ");
    uart_put(a);
    uart_put(b);
    uart_put(c);
    uart_put("\r\n");
}

/**
 * @brief Returns the text for a slot code.
 *
 * Each of the three codes has its own case. The default is only for a value that is none of
 * them (a corrupted variable; MISRA C:2012 Rule 16.4): it logs "corrupted slot code", and no longer "none",
 * so a damaged code is not read as "no trial".
 *
 * @param[in]     slot  BOOT_SLOT_A, BOOT_SLOT_B or BOOT_SLOT_NONE
 * @return "A", "B", "none", or "corrupted slot code" for any other value
 */
static const char *slot_letter(uint8_t slot)
{
    switch (slot) {
    case BOOT_SLOT_A:
        return "A";
    case BOOT_SLOT_B:
        return "B";
    case BOOT_SLOT_NONE:
        return "none";
    default:
        return "corrupted slot code";   /* outside the three codes only: see above */
    }
}

/**
 * @brief Writes the boot state to the log: confirmed slot, trial slot and failed starts.
 *
 * @param[in]     state  boot state to log
 */
static void log_state(const boot_state_t *state)
{
    uart_put("BL: boot state: confirmed ");
    uart_put(slot_letter(state->confirmed));
    uart_put(", trial ");
    uart_put(slot_letter(state->trial));
    uart_put(", failed starts ");
    uart_put_decimal(state->failed_starts);
    uart_put("\r\n");
}

/**
 * @brief Logs the cause of the last reset, then clears the reset flags.
 *
 * Which reset brought us here; a watchdog restart is the evidence of a hung trial. Flags from
 * RCC_CSR [RM0440 Rev 9, Sec. 7.4.28, p. 327-328]. Internal resets are also driven out on the NRST
 * pin [RM0440 Rev 9, Sec. 7.1.2, p. 270], so the pin flag is looked at last.
 */
static void log_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    const char *cause = "other";

    if ((csr & RCC_CSR_IWDGRSTF) != 0U) {
        cause = "watchdog";
    } else if ((csr & RCC_CSR_BORRSTF) != 0U) {
        cause = "power-on";
    } else if ((csr & RCC_CSR_SFTRSTF) != 0U) {
        cause = "software";
    } else if ((csr & RCC_CSR_PINRSTF) != 0U) {
        cause = "reset pin";
    }
    RCC->CSR |= RCC_CSR_RMVF;   /* clear, so the next start reports only its own cause */
    log_text("start, reset by ", cause, "");
}

/**
 * @brief Logs that the image in a region was accepted, with its version and build identifier.
 *
 * Up to 32 printable characters of the build identifier (Interfaces Sec. 5.5, Figure 6).
 *
 * @param[in]     region  region the image is in; its name goes in the log
 * @param[in]     image   first byte of the image
 */
static void log_accepted(const region_t *region, const uint8_t *image)
{
    char build[IMAGE_BUILD_ID_BYTES + 1U];
    uint32_t n = 0U;

    while ((n < IMAGE_BUILD_ID_BYTES) && (image[IMAGE_OFFSET_BUILD_ID + n] != 0U)) {
        uint8_t c = image[IMAGE_OFFSET_BUILD_ID + n];
        build[n] = ((c >= (uint8_t)' ') && (c <= (uint8_t)'~')) ? (char)c : '?';
        n++;
    }
    build[n] = '\0';
    uart_put("BL: ");
    uart_put(region->name);
    uart_put(" accepted: version ");
    uart_put_decimal(image_read_u32(&image[IMAGE_OFFSET_VERSION]));
    uart_put(", build ");
    uart_put(build);
    uart_put("\r\n");
}

/**
 * @brief Logs how long the signature checks of a region took, from the cycles they used.
 *
 * Microseconds from the clock actually in use (170 MHz, or 16 MHz after a clock fault); cycles
 * only when the clock is not known.
 *
 * @param[in]     region  region that was checked; its name goes in the log
 * @param[in]     cycles  core cycles the signature checks used
 */
static void log_signature_time(const region_t *region, uint32_t cycles)
{
    uint32_t mhz = clock_hz() / HZ_PER_MHZ;

    uart_put("BL: ");
    uart_put(region->name);
    uart_put(" signature check: ");
    if (mhz == 0U) {
        uart_put_decimal(cycles);
        uart_put(" cycles only (clock not known)\r\n");
        return;
    }
    uart_put_decimal(cycles / mhz);
    uart_put(" us (");
    uart_put_decimal(cycles);
    uart_put(" cycles at ");
    uart_put_decimal(mhz);
    uart_put(" MHz)\r\n");
}

/* ---- Boot state ------------------------------------------------------------------- */

/**
 * @brief Writes the boot state to flash.
 *
 * Reloads the watchdog first, so the write (two flash operations and a read-back) starts with a
 * full time-out; nothing is reloaded inside the flash waits, so a BSY that never clears ends in
 * a watchdog reset (audit finding #3). Logs a failure.
 *
 * @param[in,out] state  boot state to write; boot_state_write sets its sequence number and check
 *                       value
 * @return true if the state was written, false if the write failed
 */
static bool save_state(boot_state_t *state)
{
    watchdog_reload();
    if (boot_state_write(state)) {
        return true;
    }
    log_text("boot state write failed", "", "");
    return false;
}

/**
 * @brief Reads the boot state from flash, logs it and returns it.
 *
 * If no valid state is stored, logs that, sets confirmed slot A, no trial and 0 failed starts,
 * tries to save that state and returns it.
 *
 * @return the stored boot state, or the default state if none is valid
 */
static boot_state_t load_state(void)
{
    boot_state_t state;

    if (boot_state_read(&state)) {
        log_state(&state);
        return state;
    }
    /* Blank (first start, or after a restore cleared it) or both copies damaged. */
    log_text("no valid boot state; set to confirmed A, no trial", "", "");
    state.confirmed = BOOT_SLOT_A;
    state.trial = BOOT_SLOT_NONE;
    state.failed_starts = 0U;
    (void)save_state(&state);
    return state;
}

/**
 * @brief Ends the trial: logs the reason, clears the trial slot and the failed starts, saves the
 * state.
 *
 * @param[in,out] state  boot state; trial and failed_starts are cleared
 * @param[in]     why    reason, for the log
 * @note The result of the save is not checked.
 */
static void give_up_trial(boot_state_t *state, const char *why)
{
    uart_put("BL: trial slot ");
    uart_put(slot_letter(state->trial));
    uart_put(" given up: ");
    uart_put(why);
    uart_put("\r\n");
    state->trial = BOOT_SLOT_NONE;
    state->failed_starts = 0U;
    (void)save_state(state);
}

/**
 * @brief Count this start of the trial firmware and store the count BEFORE it runs, so a hang or a
 * power loss during the trial counts as a failed start (REQ-016, REQ-017).
 *
 * After BOOT_STATE_MAX_STARTS starts without a confirmation the trial is given up (REQ-015). The
 * count is compared before it is incremented: a stored count that is already at or above the
 * limit (even a damaged 255) gives the trial up, and cannot wrap round to 0 and start the trial
 * again (audit finding #21).
 *
 * @param[in,out] state  boot state; failed_starts goes up by one; trial is cleared if the trial is
 *                       given up
 * @note Does nothing if no trial is set.
 */
static void count_trial_start(boot_state_t *state)
{
    if (state->trial == BOOT_SLOT_NONE) {
        return;
    }
    if (state->failed_starts >= BOOT_STATE_MAX_STARTS) {
        give_up_trial(state, "started too often without confirming itself");
        return;
    }
    state->failed_starts++;
    uart_put("BL: trial slot ");
    uart_put(slot_letter(state->trial));
    uart_put(", start ");
    uart_put_decimal(state->failed_starts);
    uart_put(" of ");
    uart_put_decimal(BOOT_STATE_MAX_STARTS);
    uart_put("\r\n");
    if (!save_state(state)) {
        /* An uncounted start could hang and restart for ever: run the confirmed slot. */
        log_text("trial not started: its start could not be counted", "", "");
        state->trial = BOOT_SLOT_NONE;
    }
}

/* ---- Candidates ------------------------------------------------------------------- */

/**
 * @brief Returns the region of slot A or of slot B; NULL for any other slot code.
 *
 * Before, every code that was not A answered the region of slot B, so a damaged or empty code
 * started slot B. The caller (candidates) leaves a NULL out of the list, so a code that names
 * no slot is never started. A valid boot state never holds such a code (common/boot_state.c,
 * fields_valid); this is the second barrier if the variable is corrupted after the read.
 *
 * @param[in]     slot  BOOT_SLOT_A or BOOT_SLOT_B
 * @return region of the slot; NULL if slot is neither A nor B
 */
static const region_t *slot_region(uint8_t slot)
{
    switch (slot) {
    case BOOT_SLOT_A:
        return &REGION_A;
    case BOOT_SLOT_B:
        return &REGION_B;
    default:
        return NULL;   /* BOOT_SLOT_NONE has no slot region, and no other code is a slot */
    }
}

/**
 * @brief Returns the log text that says why a region is started: last resort, trial or confirmed.
 *
 * @param[in]     region  region to be started
 * @param[in]     state   boot state
 * @return " (last resort)", " (trial)" or " (confirmed)"
 */
static const char *role(const region_t *region, const boot_state_t *state)
{
    if (region->slot == BOOT_SLOT_NONE) {
        return " (last resort)";
    }
    return (region->slot == state->trial) ? " (trial)" : " (confirmed)";
}

/**
 * @brief The regions to try, in order; returns how many.
 *
 * The order is the trial slot (if a trial is set), the confirmed slot, the last-resort firmware.
 * A slot code that names no slot (slot_region gives NULL) is logged and left out, so it is never
 * started; the last-resort firmware is always the last entry.
 *
 * @param[in]     state  boot state
 * @param[out]    order  regions to try, first to last; room for MAX_CANDIDATES
 * @return number of regions written to order (1 to MAX_CANDIDATES)
 */
static uint32_t candidates(const boot_state_t *state, const region_t *order[MAX_CANDIDATES])
{
    uint32_t count = 0U;

    if (state->trial != BOOT_SLOT_NONE) {
        const region_t *trial = slot_region(state->trial);
        if (trial != NULL) {
            order[count] = trial;
            count++;
        } else {
            log_text("trial slot code names no slot: left out", "", "");
        }
    }
    const region_t *confirmed = slot_region(state->confirmed);
    if (confirmed != NULL) {
        order[count] = confirmed;
        count++;
    } else {
        log_text("confirmed slot code names no slot: left out", "", "");
    }
    order[count] = &REGION_LAST_RESORT;
    count++;
    return count;
}

/**
 * @brief All checks of image_check.c, plus flash read errors seen on the way.
 *
 * Logs the signature check time, then either the reason for a refusal or the acceptance.
 *
 * @param[in]     region  region whose image is checked
 * @return true if the image passed every check, false if it was refused
 */
static bool image_passes(const region_t *region)
{
    const uint8_t *image = (const uint8_t *)(uintptr_t)region->start;
    uint32_t signed_bytes = 0U;

    flash_ecc_clear();
    image_result_t result =
        image_check_contents(image, region->start, region->bytes, region->kind, &signed_bytes);
    if (result == IMAGE_OK) {
        uint32_t begin = cycles_now();
        result = image_check_signatures(image, signed_bytes);
        log_signature_time(region, cycles_now() - begin);
    }
    if (flash_ecc_error_seen()) {
        log_text(region->name, " refused: ", "flash read error (ECC)");
        return false;
    }
    if (result != IMAGE_OK) {
        log_text(region->name, " refused: ", image_result_text(result));
        return false;
    }
    log_accepted(region, image);
    return true;
}

/**
 * @brief Chooses the firmware to run and starts it.
 *
 * 0. Starts the independent watchdog with the long time-out (about 32 s) before anything else,
 *    so that nothing after this line can stop the chip for good: a flash BSY that never clears
 *    or the endless loop of a fault handler ends in a watchdog reset (audit findings #3, #4(a)(b)
 *    and #5, decision A). The watchdog is reloaded before the boot-state writes (save_state),
 *    before each candidate's checks and in the park loop, never inside a wait.
 * 1. Starts the cycle counter and checks it counts, sets the clock to 170 MHz, starts the UART,
 *    logs the reset cause. A clock fault or a stopped cycle counter is logged and the bootloader
 *    carries on.
 * 2. Loads the boot state and counts this start if a trial is set.
 * 3. Takes the regions in order: trial slot (if set), confirmed slot, last-resort firmware.
 * 4. Starts the first region whose image passes all checks.
 * 5. Gives the trial up at once if the image of the trial slot is refused.
 * 6. If no image passes: logs that, turns the UART off and parks, reloading the watchdog on every
 *    pass, so the chip stays quiet and reachable through SWD as Architecture Sec. 5.1 decided.
 *
 * @return Does not return.
 */
int main(void)
{
    watchdog_start_long();
    bool cycles_ok = cycles_start();
    bool clock_ok = clock_to_170mhz();
    uart_init();
    log_reset_cause();
    if (!cycles_ok) {
        log_text("cycle counter not counting: waits are held by their pass limits only", "", "");
    }
    if (!clock_ok) {
        /* Slower, not unsafe: the checks below are the same at 16 MHz. */
        log_text("clock fault, ", (clock_hz() == HSI16_HZ) ? "running at 16 MHz" : "clock not known", "");
    }

    boot_state_t state = load_state();
    count_trial_start(&state);

    const region_t *order[MAX_CANDIDATES];
    uint32_t count = candidates(&state, order);

    for (uint32_t i = 0U; i < count; i++) {
        watchdog_reload();   /* a full time-out for this candidate's checks */
        if (image_passes(order[i])) {
            log_text("starting ", order[i]->name, role(order[i], &state));
            handover_start(order[i]->start + (uint32_t)IMAGE_CODE_OFFSET);
        }
        /* A refused trial image will not change by itself: give the trial up now. */
        if ((state.trial != BOOT_SLOT_NONE) && (order[i]->slot == state.trial)) {
            give_up_trial(&state, "its image was refused");
        }
    }

    /* Nothing passed. Drive nothing and park, so the chip stays quiet and reachable through SWD
     * for the onboard computer's restore (Architecture Sec. 5.1). The watchdog keeps running
     * (it cannot be stopped [RM0440 Rev 9, Sec. 35.3.1, p. 1557]) and is reloaded on every pass:
     * the park is deliberate, so it must not end in a reset. */
    log_text("no firmware can run", "", "");
    uart_off();
    for (;;) {
        watchdog_reload();
    }
}
