/*
 * watchdog.c — independent watchdog (IWDG), register level: start with the long time-out,
 * reload, set the firmware's time-out.
 *
 * Why the long time-out first: before this, no watchdog ran in the bootloader, so a flash BSY
 * that never clears or an endless loop in a fault handler stopped the chip for good (audit
 * findings #3, #4(a)(b), #5). Now the IWDG runs from the first line of main(), and the chip is
 * reset when no reload comes within the time-out. The owner accepted this design (decision A).
 */
#include "watchdog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "stm32g4xx.h"

/* IWDG key values [RM0440 Rev 9, Sec. 35.4.1, p. 1559]: 0xCCCC starts the watchdog (no effect if
 * the hardware-watchdog option already started it), 0x5555 opens the PR and RLR registers for
 * writing, 0xAAAA reloads the counter. */
#define IWDG_KEY_START     0xCCCCU
#define IWDG_KEY_UNLOCK    0x5555U
#define IWDG_KEY_RELOAD    0xAAAAU

/* The counter counts down from RL to 0 at LSI / divider and resets the chip at 0
 * [RM0440 Rev 9, Sec. 35.3.1, p. 1557], so the time-out is divider x (RL + 1) / f_LSI.
 * PR = 6 (and 7) selects divider 256, PR = 3 divider 32 [RM0440 Rev 9, Sec. 35.4.2, p. 1560].
 * Long time-out: 256 x 4096 / f_LSI. The datasheet gives 32 768 ms at 32 kHz for /256 and
 * RL = 0xFFF [DS12589 Rev 6, Table 79, p. 150]; f_LSI is 29.5 to 34 kHz over -40 to 125 degC
 * [DS12589 Rev 6, Table 44, p. 105], so the time-out is 30.8 s to 35.5 s (computed:
 * 1 048 576 / 34 000 and 1 048 576 / 29 500). No bootloader step takes anywhere near that
 * between two reloads: the longest is one image check (signature check times are logged at
 * every start, bootloader/main.c, log_signature_time). */
#define IWDG_PR_DIV256         6U
#define IWDG_RELOAD_LONG       0xFFFU
/* Firmware time-out: 32 x 2000 / f_LSI = 2 s at 32 kHz, 1.88 s to 2.17 s over the LSI range
 * (computed from the formula above and DS12589 Rev 6, Table 44, p. 105). Bench value:
 * REQ-022 asks for the restart but sets no time. */
#define IWDG_PR_DIV32          3U
#define IWDG_RELOAD_FIRMWARE   1999U

/* The LSI clocks the new values in: it starts within 130 us [DS12589 Rev 6, Table 44, p. 105],
 * then each update takes "up to five cycles" [RM0440 Rev 9, Sec. 35.4.4, p. 1562]. The manual
 * does not say which clock the cycles are. Bench, 13 Oct 2026: a few ms after the long time-out
 * was written (prescaler /256), IWDG_SR read 0x00000002 (RVU set, LSIRDY set); read later it was
 * 0. That fits five cycles of the prescaled counter clock, f_LSI / divider, and not five cycles
 * of the LSI itself (170 us at 29.5 kHz). Worst case = 5 x divider / f_LSI at the slowest LSI,
 * 29.5 kHz [DS12589 Rev 6, Table 44, p. 105] (computed):
 *   divider 256 (long time-out):     5 x 256 / 29 500 = 43.4 ms
 *   divider 32  (firmware time-out): 5 x  32 / 29 500 =  5.4 ms
 * The bounds below add margin to these. They are bench values: the time the update really takes
 * is logged at every start with a trial (main.c, hold_trial_without_watchdog); confirm or
 * change them from those times. TBD for flight. */
#define IWDG_RUNNING_WAIT_MS   50U   /* long time-out: 43.4 ms worst case, 1.15 x */
#define IWDG_FIRMWARE_WAIT_MS  10U   /* firmware time-out: 5.4 ms worst case, 1.85 x */
/* Backstop for a counter that does not count, and the only limit before the cycle counter is
 * started. Every pass of a wait loop takes at least one core cycle (it holds a register read),
 * so N passes last at least N cycles. N = 2 ms of cycles at 16 MHz (HSI16, the clock after
 * reset [RM0440 Rev 9, Sec. 5.3.3, p. 190]) = 32 000 passes, which last at least 2 ms. The first
 * wait in main() is the only one with this as its only limit; the reload that follows it does
 * not depend on the update (see watchdog_start_long). The upper time of a pass is TBD (needs
 * the instruction and register-access timings of this build; not measured). */
#define IWDG_FIRST_PASSES      (2U * (HSI16_HZ / 1000U))
/* The same bounds in passes, for the waits with the cycle counter running: as many passes as
 * cycles of the bound at 170 MHz, so a working counter always ends the wait first (a pass takes
 * at least 1 cycle). */
#define IWDG_RUNNING_PASSES    (IWDG_RUNNING_WAIT_MS * (CORE_CLOCK_HZ / 1000U))
#define IWDG_FIRMWARE_PASSES   (IWDG_FIRMWARE_WAIT_MS * (CORE_CLOCK_HZ / 1000U))

/**
 * @brief Converts a time in milliseconds into cycles of the core clock now in use.
 *
 * The core is at 170 MHz, or at 16 MHz after a clock fault (clock_hz), so the cycle count of a
 * fixed time is read from the clock actually running.
 *
 * @param[in]     ms  time, milliseconds
 * @return the same time in core clock cycles
 */
static uint32_t ms_to_cycles(uint32_t ms)
{
    return ms * (clock_hz() / 1000U);
}

/**
 * @brief Waits until the IWDG has taken over the new prescaler and reload values.
 *
 * Bounded: if the update never ends the LSI is not running, and the watchdog with it; waiting
 * longer would only stop the start-up. The limit is a number of passes, and also, when
 * use_cycles is true, max_cycles on the cycle counter, whichever comes first. The first call in
 * main() passes false: the cycle counter is not started yet and its registers are not read.
 *
 * @param[in]     use_cycles  true to also bound the wait by the cycle counter
 * @param[in]     passes      most passes of the wait loop
 * @param[in]     max_cycles  most core clock cycles to wait; unused if use_cycles is false
 * @return the core clock cycles waited, as counted; 0 if use_cycles is false
 */
static uint32_t wait_update(bool use_cycles, uint32_t passes, uint32_t max_cycles)
{
    uint32_t start = use_cycles ? cycles_now() : 0U;
    uint32_t waited = 0U;

    for (uint32_t n = 0U; n < passes; n++) {
        if (IWDG->SR == 0U) {
            break;
        }
        if (use_cycles) {
            waited = cycles_now() - start;
            if (waited > max_cycles) {
                break;
            }
        }
    }
    return waited;
}

/**
 * @brief Starts the IWDG with the long time-out, about 32 s.
 *
 * The sequence is start key, unlock key, prescaler, reload value, wait for the update, reload
 * [RM0440 Rev 9, Sec. 35.3.1, p. 1557, steps 1-6, window option unused]. It also works when the
 * hardware-watchdog option bit has already started the watchdog at power-on with a short
 * time-out [RM0440 Rev 9, Sec. 35.3.3, p. 1558]: the start key then has no effect
 * [RM0440 Rev 9, Sec. 35.4.1, p. 1559], and the unlock, prescaler, reload value and reload
 * that follow replace the short time-out. Called as the first action in main(), so that
 * time-out has to last only until here.
 */
void watchdog_start_long(void)
{
    IWDG->KR = IWDG_KEY_START;
    IWDG->KR = IWDG_KEY_UNLOCK;
    IWDG->PR = IWDG_PR_DIV256;
    IWDG->RLR = IWDG_RELOAD_LONG;
    /* (void): no cycle counter yet, so no time to use. */
    (void)wait_update(false, IWDG_FIRST_PASSES, 0U);
    IWDG->KR = IWDG_KEY_RELOAD;
}

/**
 * @brief Reloads the IWDG counter: a full time-out from now.
 *
 * Called between steps of the bootloader, never inside a wait loop, so a wait that never ends
 * is ended by the reset [RM0440 Rev 9, Sec. 35.4.1, p. 1559: the key is written at regular
 * intervals, otherwise the watchdog resets when the counter reaches 0].
 */
void watchdog_reload(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}

/**
 * @brief Returns true if the watchdog is confirmed running: its register updates have settled and
 * the LSI that clocks it is ready.
 *
 * Decision F(b), owner, 13 Oct 2026: a trial slot is started only under a running watchdog,
 * because the watchdog restart is what ends a hung trial and lets the bootloader count it as a
 * failed start (REQ-014, REQ-016, REQ-022). The two bits:
 *   - IWDG_SR = 0: PVU, RVU and WVU are clear, so the prescaler and reload values written by
 *     watchdog_start_long have reached the counter's own voltage domain [RM0440 Rev 9,
 *     Sec. 35.4.4, p. 1562]; that transfer is clocked by the LSI, so it ends only while the LSI
 *     runs;
 *   - LSIRDY = 1 in RCC_CSR: the LSI oscillator is stable [RM0440 Rev 9, Sec. 7.4.28, p. 328]. A
 *     started IWDG forces the LSI on [RM0440 Rev 9, Sec. 7.2.14, p. 282].
 * Why it waits: the first version read IWDG_SR once, and the bench (13 Oct 2026) printed
 * "IWDG_SR 0x00000002, RCC_CSR 0x00000002": RVU still set a few ms after the write, so the trial
 * was never started although the watchdog ran; read later, IWDG_SR was 0. Each update takes "up
 * to five cycles" [RM0440 Rev 9, Sec. 35.4.4, p. 1562] of the prescaled counter clock, which is
 * 43.4 ms at the slowest LSI with prescaler /256 (arithmetic at IWDG_RUNNING_WAIT_MS). So the
 * function waits for IWDG_SR = 0 up to IWDG_RUNNING_WAIT_MS (50 ms, a bench value, TBD for
 * flight) on the cycle counter, which is running here, and also up to a pass limit in case the
 * counter is dead. Then it reads IWDG_SR and RCC_CSR once more and hands the two raw values and
 * the time waited back, so the caller can log exactly what the decision was made on.
 *
 * @param[out]    iwdg_sr    IWDG_SR as read at the end of the wait
 * @param[out]    rcc_csr    RCC_CSR as read at the end of the wait
 * @param[out]    waited_us  time the wait took, microseconds, from the cycle counter (0 if the
 *                           counter does not count)
 * @return true if both bits say the watchdog runs, false otherwise (also for a null pointer,
 *         with nothing waited and nothing stored)
 */
bool watchdog_running(uint32_t *iwdg_sr, uint32_t *rcc_csr, uint32_t *waited_us)
{
    if ((iwdg_sr == NULL) || (rcc_csr == NULL) || (waited_us == NULL)) {
        return false;
    }
    uint32_t waited = wait_update(true, IWDG_RUNNING_PASSES, ms_to_cycles(IWDG_RUNNING_WAIT_MS));

    *iwdg_sr = IWDG->SR;
    *rcc_csr = RCC->CSR;
    *waited_us = waited / (clock_hz() / HZ_PER_MHZ);
    return (*iwdg_sr == 0U) && ((*rcc_csr & RCC_CSR_LSIRDY) != 0U);
}

/**
 * @brief Sets the firmware's time-out of about 2 s on the running IWDG, then reloads.
 *
 * Unlock key, prescaler /32, reload value 1999, bounded wait for the update, reload
 * [RM0440 Rev 9, Sec. 35.3.1, p. 1557]. The watchdog is already running (watchdog_start_long),
 * so no start key is written. The wait is IWDG_FIRMWARE_WAIT_MS (10 ms): the update takes up to
 * five cycles of the prescaled clock [RM0440 Rev 9, Sec. 35.4.4, p. 1562], 5 x 32 / 29 500 =
 * 5.4 ms at the slowest LSI [DS12589 Rev 6, Table 44, p. 105] with prescaler /32 (the bench
 * showed the /256 update outlasting a few ms, see watchdog_running). The earlier bound, 2 ms,
 * was shorter than that. If the update does not end in time the wait gives up and the reload is
 * written anyway; the firmware then starts with whichever time-out the chip has taken over.
 */
void watchdog_set_firmware_timeout(void)
{
    IWDG->KR = IWDG_KEY_UNLOCK;
    IWDG->PR = IWDG_PR_DIV32;
    IWDG->RLR = IWDG_RELOAD_FIRMWARE;
    /* (void): the time waited is not used; the hand-over logs nothing after this. */
    (void)wait_update(true, IWDG_FIRMWARE_PASSES, ms_to_cycles(IWDG_FIRMWARE_WAIT_MS));
    IWDG->KR = IWDG_KEY_RELOAD;
}
