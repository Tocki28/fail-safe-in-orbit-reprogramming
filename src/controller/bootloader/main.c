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
 * The owner's decisions of 13 Oct 2026 carried out here (letters of the controller audit):
 *   B     restarts of a firmware started as the confirmed slot are counted in the TAMP backup
 *         registers; after CONFIRMED_RESTARTS_MAX in a row that slot is skipped and the
 *         last-resort firmware starts (count_restart, confirmed_skipped; REQ-018);
 *   D     a blank boot state gets the default, a damaged one starts only the last-resort
 *         firmware (load_state);
 *   F(b)  a trial slot is started only under a watchdog confirmed running
 *         (hold_trial_without_watchdog);
 *   G     the firmware's give-up request is applied before the start is counted
 *         (apply_give_up_request);
 *   I     the give-up and the default-state saves are tried twice and the result is logged
 *         (save_state_retry);
 *   check-result reporting: a copy refused before the one started is left in the backup
 *         registers with its check result, for the firmware's EVENT 0x0002 or 0x0003
 *         (leave_fallback_record; Interfaces Sec. 5.4);
 *   4     every copy refused or skipped on one start is recorded, up to three: a flash read
 *         error, a trial given up at its start limit and a confirmed slot skipped by decision B
 *         have their own check results (0x0B, 0x0C, 0x0D; add_record), and so do a trial held
 *         back without a confirmed watchdog, a trial whose start count could not be saved and
 *         the two slots passed over when the boot state is damaged (0x0E, 0x0F, 0x10);
 *   faults: a value in memory that cannot exist calls fault_impossible_value (common/fault.h)
 *         with the fault-location code of its function (decision 3, common/fault_codes.h).
 * And the owner's decision 30, 15 Oct 2026 (finding F-1: a SWITCH restarted a firmware that had
 * been given up; REQ-019): every give-up of a trial here also sets the FALLBACK flag and stores
 * the given-up image's body check value in the boot state (give_up_trial), so the firmware that
 * starts next runs in FALLBACK and refuses a SWITCH to that image.
 *
 * Switches over an enum have no default label. No default: the compiler checks every case
 * (-Wswitch, -Werror); only a corrupted value reaches the line after the switch. This deviates on
 * purpose from MISRA C:2012 Rule 16.4 (a default in every switch); the rule's purpose, handling a
 * value outside the cases, is met by the fault reaction after the switch. The switches over a
 * slot code (a uint8_t from boot_state.h, not an enum) follow the same pattern.
 *
 * Every line on USART2 starts with "BL: ".
 *
 * The CRC-32, the flash access, the boot state and the image checks are the bootloader's own
 * (bl_crc32.c, bl_flash.c, bl_boot_state.c, bl_image_check.c), written apart from the firmware's
 * common/ files: REQ-119, owner's decision 35, 15 Oct 2026. A defect in shared code would reach
 * the firmware and the bootloader that must recover from it at once. From common/ this file
 * takes definitions only (types, codes, the flash map).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "backup.h"
#include "backup_layout.h"
#include "bl_boot_state.h"
#include "bl_flash.h"
#include "bl_image_check.h"
#include "clock.h"
#include "fault.h"
#include "handover.h"
#include "image_format.h"
#include "layout.h"
#include "stm32g4xx.h"
#include "uart.h"
#include "watchdog.h"

/* Trial slot, confirmed slot, last-resort. */
#define MAX_CANDIDATES          3U
/* Restarts in a row of the firmware started as the confirmed slot, after which that slot is
 * skipped and the last-resort firmware starts. Decision B (owner, 13 Oct 2026): "after 3 such
 * restarts". The SRS gives REQ-018 no number of its own. */
#define CONFIRMED_RESTARTS_MAX  3U
/* The count is one byte of the RESTARTS pair (common/backup_layout.h), so it stops at 255. */
#define RESTARTS_COUNT_MAX      0xFFU

/* The check-result codes (CHECK_...) moved to common/backup_layout.h with decision 4, so the
 * firmware that reads the fall-back record checks against the same table. */

typedef struct {
    const char *name;   /* for the log */
    uint32_t start;     /* first byte of the region */
    uint32_t bytes;     /* size of the region */
    uint8_t kind;       /* the only image kind this region may run */
    uint8_t slot;       /* BOOT_SLOT_A/B, or BOOT_SLOT_NONE for the last-resort region */
    uint8_t copy;       /* its code in the fall-back record: the HEALTH report's running-slot
                         * code (Interfaces Sec. 5.4): BACKUP_COPY_A, _B or _LAST_RESORT */
    uint8_t record_shift;   /* where its check result sits in the fall-back record
                             * (BACKUP_FALLBACK_..._SHIFT, common/backup_layout.h) */
} region_t;

static const region_t REGION_A = {"slot A", SLOT_A_START, SLOT_BYTES, IMAGE_KIND_MAIN, BOOT_SLOT_A,
                                  BACKUP_COPY_A, BACKUP_FALLBACK_A_SHIFT};
static const region_t REGION_B = {"slot B", SLOT_B_START, SLOT_BYTES, IMAGE_KIND_MAIN, BOOT_SLOT_B,
                                  BACKUP_COPY_B, BACKUP_FALLBACK_B_SHIFT};
static const region_t REGION_LAST_RESORT = {"last-resort", LAST_RESORT_START, LAST_RESORT_BYTES,
                                            IMAGE_KIND_LAST_RESORT, BOOT_SLOT_NONE,
                                            BACKUP_COPY_LAST_RESORT,
                                            BACKUP_FALLBACK_LAST_RESORT_SHIFT};

/* Defined with the candidates below; count_trial_start needs it first, for the record of a trial
 * given up at its start limit. */
static const region_t *slot_region(uint8_t slot);

/**
 * @brief Adds a copy refused or skipped on this start, with its check result, to the fall-back
 * record, which is kept in RAM until the hand-over or the park (decision 4, owner, 13 Oct 2026).
 *
 * Each copy has its own byte (region_t.record_shift); a start tries each copy at most once, so no
 * record overwrites another (common/backup_layout.h, FALLBACK).
 *
 * @param[in,out] records  the fall-back record of this start
 * @param[in]     region   the copy refused or skipped
 * @param[in]     check    its check result (CHECK_..., common/backup_layout.h), 0x01 to CHECK_LAST
 */
static void add_record(uint32_t *records, const region_t *region, uint8_t check)
{
    *records |= (uint32_t)check << region->record_shift;
}

/* The cause of the last reset, from the flags of RCC_CSR [RM0440 Rev 9, Sec. 7.4.28,
 * p. 327-328]. */
typedef enum {
    RESET_WATCHDOG = 0,   /* IWDGRSTF */
    RESET_POWER_ON,       /* BORRSTF */
    RESET_SOFTWARE,       /* SFTRSTF */
    RESET_PIN,            /* PINRSTF only */
    RESET_OTHER           /* none of these four */
} reset_cause_t;

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
 * @brief Returns the text for a slot code: "A", "B" or "none".
 *
 * Every caller passes a code from a boot state that passed its checks (bl_boot_state.c,
 * fields_allowed) or that this file set itself, so any other code is a corrupted value in memory
 * and gets the fault reaction (owner's decision, 13 Oct 2026; the pattern is in the file
 * comment). Before, it returned "corrupted slot code" (commit b18e3e9), and before that
 * "unknown": a log line that went on as if the value were data. The fault reaction's own line
 * now says "corrupted value in slot_letter".
 *
 * @param[in]     slot  BOOT_SLOT_A, BOOT_SLOT_B or BOOT_SLOT_NONE
 * @return "A", "B" or "none"
 * @note Does not return for any other code.
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
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_SLOT_LETTER, "slot_letter");
}

/**
 * @brief Returns the text for a reset cause, for the start line.
 *
 * Every reset_cause_t value has its own case; a value outside the enum is a corrupted variable
 * (the pattern is in the file comment).
 *
 * @param[in]     cause  the cause read_reset_cause found
 * @return "watchdog", "power-on", "software", "reset pin" or "other"
 * @note Does not return for a value outside reset_cause_t.
 */
static const char *reset_cause_text(reset_cause_t cause)
{
    switch (cause) {
    case RESET_WATCHDOG:
        return "watchdog";
    case RESET_POWER_ON:
        return "power-on";
    case RESET_SOFTWARE:
        return "software";
    case RESET_PIN:
        return "reset pin";
    case RESET_OTHER:
        return "other";
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_RESET_CAUSE_TEXT, "reset_cause_text");
}

/**
 * @brief Writes the boot state to the log: confirmed slot, trial slot, failed starts, flags and
 * the given-up image's body check value.
 *
 * The flags and the given-up check (owner's decision 30, 15 Oct 2026) are logged as numbers, so a
 * bench run shows the FALLBACK flag and the mark that a SWITCH is refused on.
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
    uart_put(", flags ");
    uart_put_decimal(state->flags);
    uart_put(", given up ");
    uart_put_hex32(state->given_up_check);
    uart_put("\r\n");
}

/**
 * @brief Reads the cause of the last reset, clears the reset flags, logs the cause and returns
 * it.
 *
 * Which reset brought us here; a watchdog restart is the evidence of a hung firmware, and decision
 * B counts it. The flags are read before RMVF clears them (decision B). Flags from RCC_CSR
 * [RM0440 Rev 9, Sec. 7.4.28, p. 327-328]. Internal resets are also driven out on the NRST pin
 * [RM0440 Rev 9, Sec. 7.1.2, p. 270], so the pin flag is looked at last.
 *
 * @return the cause of the last reset
 */
static reset_cause_t read_reset_cause(void)
{
    uint32_t csr = RCC->CSR;
    reset_cause_t cause = RESET_OTHER;

    if ((csr & RCC_CSR_IWDGRSTF) != 0U) {
        cause = RESET_WATCHDOG;
    } else if ((csr & RCC_CSR_BORRSTF) != 0U) {
        cause = RESET_POWER_ON;
    } else if ((csr & RCC_CSR_SFTRSTF) != 0U) {
        cause = RESET_SOFTWARE;
    } else if ((csr & RCC_CSR_PINRSTF) != 0U) {
        cause = RESET_PIN;
    }
    RCC->CSR |= RCC_CSR_RMVF;   /* clear, so the next start reports only its own cause */
    log_text("start, reset by ", reset_cause_text(cause), "");
    return cause;
}

/**
 * @brief Logs that the image in a region was accepted, with its version and build identifier.
 *
 * Up to 32 printable characters of the build identifier (Interfaces Sec. 5.5, Figure 6).
 *
 * @param[in]     region  region the image is in; its name goes in the log
 */
static void log_accepted(const region_t *region)
{
    const uint8_t *image = bl_flash_at(region->start);
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
    uart_put_decimal(bl_image_read_u32(&image[IMAGE_OFFSET_VERSION]));
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
 * @param[in,out] state  boot state to write; bl_boot_state_write sets its sequence number and
 *                       check value
 * @return true if the state was written, false if the write failed
 */
static bool save_state(boot_state_t *state)
{
    watchdog_reload();
    if (bl_boot_state_write(state)) {
        return true;
    }
    log_text("boot state write failed", "", "");
    return false;
}

/**
 * @brief Writes the boot state; if that fails, tries once more; logs the result (decision I).
 *
 * Why a second try: a write can fail once and pass the next time (an error flag left set, a
 * power dip during the erase); bl_flash.c clears the error flags before every operation, and
 * bl_boot_state_write writes the copy that is not current again, so the second try starts clean.
 * Why only one: a flash that refuses twice will not be cured by more tries, and every try stalls
 * the start by up to one page erase, 24.47 ms [DS12589 Rev 6, Table 46, p. 107].
 *
 * @param[in,out] state  boot state to write; sequence number and check value are set
 * @return true if the first or the second write succeeded, false if both failed
 */
static bool save_state_retry(boot_state_t *state)
{
    if (save_state(state)) {
        return true;
    }
    log_text("boot state write: trying once more", "", "");
    if (save_state(state)) {
        log_text("boot state written on the second try", "", "");
        return true;
    }
    log_text("boot state write failed again: the change holds for this start only", "", "");
    return false;
}

/**
 * @brief Reads the boot state and decides from it (decision D); logs what it found.
 *
 * VALID: the stored state is used. BLANK (both copies erased, as on a fresh chip or after a
 * restore): confirmed A, no trial, 0 failed starts, as before, no FALLBACK flag and no given-up
 * image (flags 0, given_up_check 0: owner's decision 30, 15 Oct 2026; nothing was ever given up
 * on an erased state); it is saved, tried twice (decision I). DAMAGED (no valid copy, and a copy
 * that is not erased): false, so main() starts only the last-resort firmware. Why: the stored
 * state is lost, and the default could start a slot the operator had given up (REQ-019); the
 * last-resort firmware is the one copy that needs no boot state (Architecture Sec. 5.1).
 *
 * Every boot_state_load_t value has its own case (the pattern is in the file comment).
 *
 * @param[out]    state  the boot state to use; not meaningful when false is returned
 * @return true if a state is to be used (VALID or BLANK), false if the boot state is damaged
 * @note Does not return for a value outside boot_state_load_t.
 */
static bool load_state(boot_state_t *state)
{
    switch (bl_boot_state_load(state)) {
    case BOOT_STATE_VALID:
        log_state(state);
        return true;
    case BOOT_STATE_BLANK:
        log_text("boot state blank (both copies erased); set to confirmed A, no trial", "", "");
        state->confirmed = BOOT_SLOT_A;
        state->trial = BOOT_SLOT_NONE;
        state->failed_starts = 0U;
        state->flags = 0U;
        state->given_up_check = 0U;
        /* The result is logged by save_state_retry. The default is used for this start either
         * way; if it was not stored, the next start finds the copies blank and sets it again. */
        (void)save_state_retry(state);
        return true;
    case BOOT_STATE_DAMAGED:
        log_text("boot state damaged (no valid copy, a copy not erased): last-resort only", "",
                 "");
        return false;
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_LOAD_STATE, "load_state");
}

/**
 * @brief Returns the body check value in the header of the image in a region, or 0 when the
 * header's proprietary string is not there or its read saw a flash read error.
 *
 * The value that marks a given-up image in the boot state (given_up_check; owner's decision 30,
 * 15 Oct 2026). Only the header is read, at its fixed offsets (common/image_format.h; Interfaces
 * Sec. 5.5, Figure 6): the image itself may be the one just refused, so nothing else of it is
 * trusted, and no length is read. 0 for a header without the proprietary string, as the decision
 * says: such a region holds no image that could be started. A value read with a two-bit flash
 * error is not the header's value, so it is 0 too: Proposal (TBR), awaiting the owner's
 * acceptance (a region read with that error is refused by the image checks before any start).
 *
 * @param[in]     region  region of the image (slot A or slot B)
 * @return the header's body check value, or 0
 */
static uint32_t given_up_check_of(const region_t *region)
{
    const uint8_t *image = bl_flash_at(region->start);

    bl_flash_ecc_clear();
    bool magic = (image[IMAGE_OFFSET_MAGIC] == IMAGE_MAGIC_0) &&
                 (image[IMAGE_OFFSET_MAGIC + 1U] == IMAGE_MAGIC_1) &&
                 (image[IMAGE_OFFSET_MAGIC + 2U] == IMAGE_MAGIC_2) &&
                 (image[IMAGE_OFFSET_MAGIC + 3U] == IMAGE_MAGIC_3);
    uint32_t check = bl_image_read_u32(&image[IMAGE_OFFSET_BODY_CHECK]);
    if (!magic || bl_flash_ecc_error_seen()) {
        return 0U;
    }
    return check;
}

/**
 * @brief Ends the trial: logs the reason, clears the trial slot and the failed starts, sets the
 * FALLBACK flag and the given-up image's mark, saves the state, tried twice (decision I).
 *
 * The confirmed slot stays as it is: it is the previous slot, which starts in the trial's place.
 * The FALLBACK flag and the mark (owner's decision 30, 15 Oct 2026; finding F-1, REQ-019): the
 * firmware that starts next reads the flag and runs in FALLBACK, and it refuses a SWITCH to an
 * image whose body check value equals the mark (firmware/mode-manager/mode_manager.cpp). The mark
 * is read from the trial slot's header before the trial field is cleared (given_up_check_of).
 * Every give-up of this file comes here: the start count used up (count_trial_start), the trial
 * image refused (start_first_passing), and the firmware's give-up request (apply_give_up_request).
 *
 * @param[in,out] state  boot state; trial and failed_starts are cleared, flags and given_up_check
 *                       are set
 * @param[in]     why    reason, for the log
 * @return true if the state was saved, false if both writes failed (the trial is then given up
 *         in RAM only, for this start)
 */
static bool give_up_trial(boot_state_t *state, const char *why)
{
    uart_put("BL: trial slot ");
    uart_put(slot_letter(state->trial));
    uart_put(" given up: ");
    uart_put(why);
    uart_put("\r\n");
    state->given_up_check = given_up_check_of(slot_region(state->trial));
    state->flags = BOOT_STATE_FLAG_FALLBACK;
    state->trial = BOOT_SLOT_NONE;
    state->failed_starts = 0U;
    return save_state_retry(state);
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
 * A trial given up here is recorded with check result 0x0C, "trial started too often"
 * (decision 4, owner, 13 Oct 2026): it is the first candidate, so whatever copy starts on this
 * start starts in its place. Before, it left no record. A trial whose start count could not be
 * saved is not started and is recorded with 0x0F, "its start count could not be saved" (owner,
 * 13 Oct 2026, second round); before, it left no record either.
 *
 * @param[in,out] state    boot state; failed_starts goes up by one; trial is cleared if the trial
 *                         is given up
 * @param[in,out] records  the fall-back record of this start (add_record)
 * @note Does nothing if no trial is set.
 */
static void count_trial_start(boot_state_t *state, uint32_t *records)
{
    if (state->trial == BOOT_SLOT_NONE) {
        return;
    }
    if (state->failed_starts >= BOOT_STATE_MAX_STARTS) {
        add_record(records, slot_region(state->trial), CHECK_TRIAL_STARTS);
        /* Not started either way. If the save failed twice, the stored count is still at the
         * limit, so the next start gives the trial up again. */
        (void)give_up_trial(state, "started too often without confirming itself");
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
        add_record(records, slot_region(state->trial), CHECK_START_NOT_COUNTED);
        state->trial = BOOT_SLOT_NONE;
    }
}

/**
 * @brief Applies the firmware's give-up request, if one is stored: the trial it names is given up
 * before this start is counted (decision G).
 *
 * The firmware leaves the request when its own boot-state write of a give-up fails
 * (firmware/mode-manager/mode_manager.cpp, giveUp). Applied here, the trial is not started
 * again, and its start is not counted as a failed start. The request is cleared once the give-up
 * is saved; if the save fails twice it is kept, so the next start applies it again (the trial is
 * not started on this start either: give_up_trial cleared it in RAM). A request that names no
 * trial of this boot state (the boot-state write had succeeded after all, or the record is
 * stale) is dropped. A broken pair reads as no request (bootloader/backup.c).
 *
 * @param[in,out] state  boot state; the trial is given up if the request names it
 */
static void apply_give_up_request(boot_state_t *state)
{
    uint32_t request = 0U;

    if (!backup_read(BACKUP_PAIR_GIVE_UP, &request) || (request == 0U)) {
        return;
    }
    if ((state->trial != BOOT_SLOT_NONE) && (request == state->trial)) {
        if (!give_up_trial(state, "the firmware's give-up request")) {
            log_text("give-up request kept for the next start", "", "");
            return;
        }
    } else {
        /* The stored value is printed as a number: it is data from the backup registers, not a
         * checked slot code, so slot_letter (fault on any other code) is not asked. */
        uart_put("BL: give-up request for slot code ");
        uart_put_decimal(request);
        uart_put(" names no trial: dropped\r\n");
    }
    if (!backup_write(BACKUP_PAIR_GIVE_UP, 0U)) {
        log_text("give-up request could not be cleared", "", "");
    }
}

/**
 * @brief Holds a trial back for this start if the watchdog is not confirmed running
 * (decision F(b)).
 *
 * A hung trial is ended only by the watchdog restart, which the next start counts as a failed
 * start (REQ-014, REQ-016); without a running watchdog a hang would stop the chip for good. So
 * the confirmed slot starts instead, and the log says why. Called before count_trial_start, so
 * the held-back start is not counted, and in RAM only: the stored boot state keeps the trial,
 * which the next start tries again.
 *
 * The log line carries the raw IWDG_SR and RCC_CSR that the decision was made on (read by
 * watchdog_running) and the time it waited. Cause found on the bench (13 Oct 2026, scenario 2):
 * the check read IWDG_SR 0x00000002 (reload update still running) a few ms after the write, and
 * the update takes up to 43.4 ms, so watchdog_running now waits for it. When the trial is
 * started, one line gives the time the update really took, so the bound can be confirmed from
 * the board (bench value, TBD for flight; see watchdog.c).
 *
 * The held-back trial is recorded with check result 0x0E, "not started: the watchdog was not
 * confirmed running" (owner, 13 Oct 2026, second round): the confirmed slot starts in its place,
 * and the firmware that runs reports the fall-back. Before, it left no record.
 *
 * @param[in,out] state    boot state; trial is cleared in RAM if the watchdog is not running
 * @param[in,out] records  the fall-back record of this start (add_record)
 */
static void hold_trial_without_watchdog(boot_state_t *state, uint32_t *records)
{
    uint32_t iwdg_sr = 0U;
    uint32_t rcc_csr = 0U;
    uint32_t waited_us = 0U;

    if (state->trial == BOOT_SLOT_NONE) {
        return;
    }
    if (watchdog_running(&iwdg_sr, &rcc_csr, &waited_us)) {
        uart_put("BL: watchdog running (update settled in ");
        uart_put_decimal(waited_us);
        uart_put(" us)\r\n");
        return;
    }
    uart_put("BL: trial slot ");
    uart_put(slot_letter(state->trial));
    uart_put(" not started: watchdog not confirmed running (IWDG_SR ");
    uart_put_hex32(iwdg_sr);
    uart_put(", RCC_CSR ");
    uart_put_hex32(rcc_csr);
    uart_put(", waited ");
    uart_put_decimal(waited_us);
    uart_put(" us)\r\n");
    add_record(records, slot_region(state->trial), CHECK_WATCHDOG_NOT_RUNNING);
    state->trial = BOOT_SLOT_NONE;
}

/* ---- Restarts of the confirmed slot (decision B) ------------------------------------ */

/**
 * @brief Counts the restart that brought us here if decision B counts it, and clears the records
 * of the start that just ended.
 *
 * Counted: a watchdog restart, or a software restart that the firmware marked as its fault
 * reaction (FAULT pair: common/fault.h, or a failed self-test, decision L), of a firmware the
 * bootloader had started as the confirmed slot (LAST_START pair). The count belongs to one slot:
 * a counted restart of the other slot starts again at 1. Not counted:
 *   - restarts of a trial (it has its own count of failed starts, REQ-015) or of the last-resort
 *     firmware (nothing to fall back to);
 *   - a power-on, a reset-pin reset, and a software restart without the mark (SWITCH, a trial's
 *     give-up), which are not loops of a failing firmware;
 *   - any restart while the bootloader itself ran: LAST_START is cleared here at every start and
 *     written only just before the hand-over (note_start), so a hang or a fault of the
 *     bootloader is never charged to a slot.
 * The firmware clears the count after running normally for a set time (kNormalRunMs, 60 s bench
 * value, firmware/mode-manager/mode_manager.hpp). A LAST_START value whose role or slot is none
 * of the codes is stored data that failed its check: not counted, never a fault
 * (common/backup_layout.h).
 *
 * @param[in]     cause  the cause of the last reset (read_reset_cause)
 */
static void count_restart(reset_cause_t cause)
{
    uint32_t last = 0U;
    uint32_t fault = 0U;
    bool last_known = backup_read(BACKUP_PAIR_LAST_START, &last);
    bool fault_marked = backup_read(BACKUP_PAIR_FAULT, &fault) && (fault != 0U);
    bool cleared = backup_write(BACKUP_PAIR_LAST_START, 0U);

    cleared = backup_write(BACKUP_PAIR_FAULT, 0U) && cleared;
    if (!cleared) {
        log_text("backup registers not writable: restarts of the confirmed slot not counted", "",
                 "");
    }
    if (fault_marked) {
        uart_put("BL: the firmware restarted itself after a fault, reason ");
        uart_put_decimal(fault);
        uart_put("\r\n");
    }

    uint32_t role = (last >> BACKUP_START_ROLE_SHIFT) & BACKUP_BYTE_MASK;
    uint32_t slot = last & BACKUP_BYTE_MASK;
    bool counted_cause = (cause == RESET_WATCHDOG) || ((cause == RESET_SOFTWARE) && fault_marked);
    if (!counted_cause || !last_known || (role != BACKUP_ROLE_CONFIRMED) ||
        ((slot != BOOT_SLOT_A) && (slot != BOOT_SLOT_B))) {
        return;
    }

    uint32_t record = 0U;
    uint32_t count = 0U;
    if (backup_read(BACKUP_PAIR_RESTARTS, &record) &&
        (((record >> BACKUP_RESTARTS_SLOT_SHIFT) & BACKUP_BYTE_MASK) == slot)) {
        count = record & BACKUP_BYTE_MASK;
    }
    if (count < RESTARTS_COUNT_MAX) {
        count++;
    }
    uart_put("BL: confirmed slot ");
    uart_put(slot_letter((uint8_t)slot));
    uart_put(" restarted by ");
    uart_put((cause == RESET_WATCHDOG) ? "the watchdog" : "its fault reaction");
    uart_put(": ");
    uart_put_decimal(count);
    uart_put(" in a row\r\n");
    if (!backup_write(BACKUP_PAIR_RESTARTS, (slot << BACKUP_RESTARTS_SLOT_SHIFT) | count)) {
        log_text("restart count could not be stored", "", "");
    }
}

/**
 * @brief Returns true if the confirmed slot is to be skipped: its restarts in a row have reached
 * CONFIRMED_RESTARTS_MAX (decision B, REQ-018). Logs it.
 *
 * Skipped means the candidates go on to the last-resort firmware (a set trial still comes first,
 * with its own count). Only the count of the slot now confirmed matters: a record of the other
 * slot is old. A broken pair reads as no count.
 *
 * @param[in]     state  boot state
 * @return true if the confirmed slot is skipped on this start, false otherwise
 */
static bool confirmed_skipped(const boot_state_t *state)
{
    uint32_t record = 0U;

    if (!backup_read(BACKUP_PAIR_RESTARTS, &record)) {
        return false;
    }
    uint32_t slot = (record >> BACKUP_RESTARTS_SLOT_SHIFT) & BACKUP_BYTE_MASK;
    uint32_t count = record & BACKUP_BYTE_MASK;
    if ((slot != state->confirmed) || (count < CONFIRMED_RESTARTS_MAX)) {
        return false;
    }
    uart_put("BL: confirmed slot ");
    uart_put(slot_letter(state->confirmed));
    uart_put(" skipped: restarted ");
    uart_put_decimal(count);
    uart_put(" times in a row without running normally\r\n");
    return true;
}

/* ---- Candidates ------------------------------------------------------------------- */

/**
 * @brief Returns the region of slot A or of slot B; any other code is a fault.
 *
 * The callers ask only for a slot from a boot state that passed its checks (bl_boot_state.c,
 * fields_allowed: confirmed A or B, trial none or the other slot), and only for the trial when one
 * is set; the default state is set in this file. So any other code here is a corrupted value in
 * memory: the fault reaction (owner's decision, 13 Oct 2026). Before, it returned NULL and the
 * caller left the code out of the candidates, which carried on as if the value were data; before
 * that, every code that was not A gave slot B.
 *
 * @param[in]     slot  BOOT_SLOT_A or BOOT_SLOT_B
 * @return region of the slot
 * @note Does not return for any other code.
 */
static const region_t *slot_region(uint8_t slot)
{
    switch (slot) {
    case BOOT_SLOT_A:
        return &REGION_A;
    case BOOT_SLOT_B:
        return &REGION_B;
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_SLOT_REGION, "slot_region");
}

/**
 * @brief Returns the role a region is started in: last resort, trial or confirmed
 * (BACKUP_ROLE_..., common/backup_layout.h).
 *
 * @param[in]     region  region to be started
 * @param[in]     state   boot state
 * @return BACKUP_ROLE_LAST_RESORT, BACKUP_ROLE_TRIAL or BACKUP_ROLE_CONFIRMED
 */
static uint32_t role_code(const region_t *region, const boot_state_t *state)
{
    if (region->slot == BOOT_SLOT_NONE) {
        return BACKUP_ROLE_LAST_RESORT;
    }
    return (region->slot == state->trial) ? BACKUP_ROLE_TRIAL : BACKUP_ROLE_CONFIRMED;
}

/**
 * @brief Returns the log text for a role: " (last resort)", " (trial)" or " (confirmed)".
 *
 * Every role role_code() returns has its own case; any other value is a corrupted variable (the
 * pattern is in the file comment).
 *
 * @param[in]     role  BACKUP_ROLE_LAST_RESORT, BACKUP_ROLE_TRIAL or BACKUP_ROLE_CONFIRMED
 * @return the text
 * @note Does not return for any other value.
 */
static const char *role_text(uint32_t role)
{
    switch (role) {
    case BACKUP_ROLE_LAST_RESORT:
        return " (last resort)";
    case BACKUP_ROLE_TRIAL:
        return " (trial)";
    case BACKUP_ROLE_CONFIRMED:
        return " (confirmed)";
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_ROLE_TEXT, "role_text");
}

/**
 * @brief The regions to try, in order; returns how many.
 *
 * The order is the trial slot (if a trial is set), the confirmed slot (unless decision B skips
 * it), the last-resort firmware, which is always the last entry.
 *
 * @param[in]     state           boot state
 * @param[in]     skip_confirmed  true to leave the confirmed slot out (confirmed_skipped)
 * @param[out]    order           regions to try, first to last; room for MAX_CANDIDATES
 * @return number of regions written to order (1 to MAX_CANDIDATES)
 */
static uint32_t candidates(const boot_state_t *state, bool skip_confirmed,
                           const region_t *order[MAX_CANDIDATES])
{
    uint32_t count = 0U;

    if (state->trial != BOOT_SLOT_NONE) {
        order[count] = slot_region(state->trial);
        count++;
    }
    if (!skip_confirmed) {
        order[count] = slot_region(state->confirmed);
        count++;
    }
    order[count] = &REGION_LAST_RESORT;
    count++;
    return count;
}

/**
 * @brief Returns the code of the Interfaces "Check result" table for a result of the image
 * checks (Interfaces Sec. 5.4).
 *
 * The table numbers the checks in the order bl_image_check.c runs them, 0x00 for a copy that
 * passed. Every image_result_t value has its own case, written out, so a result added later
 * cannot fall into a wrong code (-Wswitch-enum); a value outside the enum is a corrupted variable
 * (the pattern is in the file comment).
 *
 * @param[in]     result  result of bl_image_check_contents or bl_image_check_signatures
 * @return CHECK_PASSED to CHECK_SIGNATURE_B
 * @note Does not return for a value outside image_result_t.
 */
static uint8_t check_result_code(image_result_t result)
{
    switch (result) {
    case IMAGE_OK:
        return CHECK_PASSED;
    case IMAGE_EMPTY:
        return CHECK_EMPTY;
    case IMAGE_BAD_MAGIC:
        return CHECK_MAGIC;
    case IMAGE_BAD_FORMAT:
        return CHECK_FORMAT;
    case IMAGE_BAD_KIND:
        return CHECK_KIND;
    case IMAGE_BAD_TARGET:
        return CHECK_TARGET;
    case IMAGE_BAD_LENGTH:
        return CHECK_LENGTH;
    case IMAGE_BAD_START:
        return CHECK_START;
    case IMAGE_BAD_BODY_CHECK:
        return CHECK_BODY;
    case IMAGE_BAD_SIGNATURE_A:
        return CHECK_SIGNATURE_A;
    case IMAGE_BAD_SIGNATURE_B:
        return CHECK_SIGNATURE_B;
    }
    /* only a corrupted value reaches here */
    fault_impossible_value(FAULT_AT_CHECK_RESULT_CODE, "check_result_code");
}

/**
 * @brief All checks of bl_image_check.c, plus flash read errors seen on the way.
 *
 * Logs the signature check time, then either the reason for a refusal or the acceptance. The
 * image is read through bl_flash_at() (bl_flash.h).
 *
 * @param[in]     region      region whose image is checked
 * @param[out]    result      the first check that failed, or IMAGE_OK
 * @param[out]    read_error  true if a flash read error (ECC) was seen during the checks
 * @return true if the image passed every check, false if it was refused
 */
static bool image_passes(const region_t *region, image_result_t *result, bool *read_error)
{
    const uint8_t *image = bl_flash_at(region->start);
    uint32_t signed_bytes = 0U;

    bl_flash_ecc_clear();
    *result =
        bl_image_check_contents(image, region->start, region->bytes, region->kind, &signed_bytes);
    if (*result == IMAGE_OK) {
        uint32_t begin = cycles_now();
        *result = bl_image_check_signatures(image, signed_bytes);
        log_signature_time(region, cycles_now() - begin);
    }
    *read_error = bl_flash_ecc_error_seen();
    if (*read_error) {
        log_text(region->name, " refused: ", "flash read error (ECC)");
        return false;
    }
    if (*result != IMAGE_OK) {
        log_text(region->name, " refused: ", bl_image_result_text(*result));
        return false;
    }
    log_accepted(region);
    return true;
}

/* ---- What the started firmware learns (backup registers) -------------------------- */

/**
 * @brief Clears the fall-back record, so it describes only this start.
 *
 * Cleared at every start, before any copy is checked: a record left by an earlier start (for the
 * last-resort firmware, say, which may not have cleared it) can then never be reported by a
 * firmware started later.
 */
static void clear_fallback_record(void)
{
    if (!backup_write(BACKUP_PAIR_FALLBACK, 0U)) {
        log_text("fall-back record could not be cleared", "", "");
    }
}

/**
 * @brief Leaves every copy refused or skipped on this start, with its check result, and the copy
 * started, for the firmware that starts (Interfaces Sec. 5.4, events 0x0002 and 0x0003, accepted
 * 13 Oct 2026; decision 4, owner, 13 Oct 2026).
 *
 * The firmware that starts sends one EVENT 0x0002 (a main slot started: fall-back to the previous
 * copy) or 0x0003 (the last-resort firmware started) per record, with the 2-byte detail "refused
 * copy, check result", once after its start, and clears the record. In this repository only the
 * main firmware does so (0x0002); the last-resort region holds the bench test application, which
 * sends no EVENT. Before decision 4, when two copies
 * were refused (trial, then confirmed), the record named only the second; now it holds both.
 * Nothing is written when no copy was refused or skipped: the pair was cleared at this start
 * (clear_fallback_record). At the park no copy started, so the started byte is 0; no program
 * reads that record, and the next start clears it.
 *
 * @param[in]     records  the fall-back record of this start (add_record)
 * @param[in]     started  the copy about to start (region_t.copy), 0 when none starts
 */
static void leave_fallback_record(uint32_t records, uint8_t started)
{
    if (records == 0U) {
        return;
    }
    if (!backup_write(BACKUP_PAIR_FALLBACK,
                      records | ((uint32_t)started << BACKUP_FALLBACK_STARTED_SHIFT))) {
        log_text("fall-back record could not be stored: no EVENT for this fall-back", "", "");
    }
}

/**
 * @brief Records what is started and in which role, for decision B's count at the next start.
 *
 * @param[in]     region  region about to start
 * @param[in]     role    BACKUP_ROLE_... (role_code)
 */
static void note_start(const region_t *region, uint32_t role)
{
    if (!backup_write(BACKUP_PAIR_LAST_START, (role << BACKUP_START_ROLE_SHIFT) | region->slot)) {
        log_text("start not recorded: a restart of this start will not be counted", "", "");
    }
}

/**
 * @brief Checks the regions in order and starts the first whose image passes.
 *
 * Before the jump: the fall-back record if a copy was refused or skipped (check-result
 * reporting, decision 4), the start record (decision B), the backup registers closed. A refused
 * trial image will not change by itself, so the trial is given up at once.
 * Every refused copy is recorded (decision 4, owner, 13 Oct 2026): with check result 0x0B if a
 * flash read raised an ECC error during its checks, which is the reason image_passes refuses it
 * whatever the checks found; otherwise with the first check it failed. Before, a copy refused
 * only for a flash read error left no record, and only the last refused copy was kept.
 * A confirmed slot skipped by decision B is recorded with 0x0D when the start goes past its place,
 * which is just before the last-resort firmware (candidates). A trial that passes starts before
 * that place, so its start leaves no record of a skip that changed nothing.
 * If no region passes, the record is left with no copy started, before the park.
 *
 * @param[in,out] state    boot state; the trial is given up if its image is refused
 * @param[in]     order    regions to try, first to last
 * @param[in]     count    number of regions in order
 * @param[in]     skipped  the confirmed slot's region if decision B skips it, NULL otherwise
 * @param[in,out] records  the fall-back record of this start (add_record)
 * @note Returns only if no region passes; otherwise it does not return (handover_start).
 */
static void start_first_passing(boot_state_t *state, const region_t *const order[],
                                uint32_t count, const region_t *skipped, uint32_t *records)
{
    for (uint32_t i = 0U; i < count; i++) {
        image_result_t result = IMAGE_OK;
        bool read_error = false;

        if ((skipped != NULL) && (order[i] == &REGION_LAST_RESORT)) {
            add_record(records, skipped, CHECK_RESTARTS);
        }
        watchdog_reload();   /* a full time-out for this candidate's checks */
        if (image_passes(order[i], &result, &read_error)) {
            uint32_t role = role_code(order[i], state);
            leave_fallback_record(*records, order[i]->copy);
            note_start(order[i], role);
            backup_close();
            log_text("starting ", order[i]->name, role_text(role));
            handover_start(order[i]->start + (uint32_t)IMAGE_CODE_OFFSET);
        }
        uint8_t check = CHECK_FLASH_READ;
        if (!read_error) {
            check = check_result_code(result);
        }
        add_record(records, order[i], check);
        if ((state->trial != BOOT_SLOT_NONE) && (order[i]->slot == state->trial)) {
            /* The trial is not started on this start either way. If both saves fail, the next
             * start checks the image again, refuses it again and gives the trial up again. */
            (void)give_up_trial(state, "its image was refused");
        }
    }
    leave_fallback_record(*records, 0U);
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
 *    reads and logs the reset cause. A clock fault or a stopped cycle counter is logged and the
 *    bootloader carries on.
 * 2. Opens the backup registers, counts a restart of the confirmed slot (decision B) and clears
 *    the fall-back record.
 * 3. Loads the boot state (decision D). Damaged: only the last-resort firmware is tried.
 *    Otherwise: applies the firmware's give-up request (decision G), holds a trial back without a
 *    running watchdog (decision F(b)), counts this start if a trial is set, and takes the regions
 *    in order: trial slot (if set), confirmed slot (unless decision B skips it), last-resort.
 * 4. Starts the first region whose image passes all checks; a refused trial is given up at once.
 *    Every copy refused or skipped on the way is left in the fall-back record (decision 4).
 * 5. If no image passes: logs that, closes the backup registers, turns the UART off and parks,
 *    reloading the watchdog on every pass, so the chip stays quiet and reachable through SWD as
 *    Architecture Sec. 5.1 decided.
 *
 * @return Does not return.
 */
int main(void)
{
    watchdog_start_long();
    bool cycles_ok = cycles_start();
    bool clock_ok = clock_to_170mhz();
    uart_init();
    reset_cause_t cause = read_reset_cause();
    if (!cycles_ok) {
        log_text("cycle counter not counting: waits are held by their pass limits only", "", "");
    }
    if (!clock_ok) {
        /* Slower, not unsafe: the checks below are the same at 16 MHz. */
        log_text("clock fault, ", (clock_hz() == HSI16_HZ) ? "running at 16 MHz" : "clock not known", "");
    }

    backup_open();
    count_restart(cause);
    clear_fallback_record();

    boot_state_t state;
    const region_t *order[MAX_CANDIDATES];
    const region_t *skipped = NULL;
    uint32_t records = 0U;
    uint32_t count;

    /* All zero: BOOT_SLOT_NONE in both slot fields, for the damaged case below. */
    memset(&state, 0, sizeof state);
    if (load_state(&state)) {
        apply_give_up_request(&state);
        hold_trial_without_watchdog(&state, &records);
        count_trial_start(&state, &records);
        if (confirmed_skipped(&state)) {
            skipped = slot_region(state.confirmed);
        }
        count = candidates(&state, skipped != NULL, order);
    } else {
        /* Slot A and slot B are passed over unchecked: recorded with 0x10, one record each, so the
         * firmware that runs reports both — the same format as every other fall-back, and it says
         * that neither slot was tried and why (owner accepted the two-event form, 13 Oct 2026). */
        add_record(&records, &REGION_A, CHECK_STATE_DAMAGED);
        add_record(&records, &REGION_B, CHECK_STATE_DAMAGED);
        order[0] = &REGION_LAST_RESORT;
        count = 1U;
    }
    start_first_passing(&state, order, count, skipped, &records);

    /* Nothing passed. Drive nothing and park, so the chip stays quiet and reachable through SWD
     * for the onboard computer's restore (Architecture Sec. 5.1). The watchdog keeps running
     * (it cannot be stopped [RM0440 Rev 9, Sec. 35.3.1, p. 1557]) and is reloaded on every pass:
     * the park is deliberate, so it must not end in a reset. */
    log_text("no firmware can run", "", "");
    backup_close();
    uart_off();
    for (;;) {   /* endless: the park, until the onboard computer restores the chip over SWD */
        watchdog_reload();
    }
}
