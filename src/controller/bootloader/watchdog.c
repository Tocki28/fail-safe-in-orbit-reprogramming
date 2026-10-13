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
 * then each update takes up to five LSI cycles [RM0440 Rev 9, Sec. 35.4.4, p. 1562], 170 us at
 * the slowest 29.5 kHz. 2 ms of cycles at 170 MHz is about six times that (21 ms at 16 MHz). */
#define IWDG_UPDATE_CYCLES     (2U * (CORE_CLOCK_HZ / 1000U))
/* Backstop for a counter that does not count, and the only limit before the cycle counter is
 * started. Every pass of a wait loop takes at least one core cycle (it holds a register read),
 * so N passes last at least N cycles. N = 2 ms of cycles at 16 MHz (HSI16, the clock after
 * reset [RM0440 Rev 9, Sec. 5.3.3, p. 190]) = 32 000 passes, which last at least 2 ms: over
 * the 300 us above. The upper time of a pass is TBD (needs the instruction and register-access
 * timings of this build; not measured). */
#define IWDG_FIRST_PASSES      (2U * (HSI16_HZ / 1000U))
/* The same bound with the cycle counter running and the core at 170 MHz: as many passes as
 * cycles, so a working counter always ends the wait first (a pass takes at least 1 cycle). */
#define IWDG_UPDATE_PASSES     IWDG_UPDATE_CYCLES

/**
 * @brief Waits until the IWDG has taken over the new prescaler and reload values.
 *
 * Bounded: if the update never ends the LSI is not running, and the watchdog with it; waiting
 * longer would only stop the start-up. The limit is a number of passes, and also, when
 * use_cycles is true, IWDG_UPDATE_CYCLES on the cycle counter, whichever comes first. The first
 * call in main() passes false: the cycle counter is not started yet and its registers are not
 * read.
 *
 * @param[in]     use_cycles  true to also bound the wait by the cycle counter
 * @param[in]     passes      most passes of the wait loop
 */
static void wait_update(bool use_cycles, uint32_t passes)
{
    uint32_t start = use_cycles ? cycles_now() : 0U;

    for (uint32_t n = 0U; n < passes; n++) {
        if (IWDG->SR == 0U) {
            return;
        }
        if (use_cycles && ((cycles_now() - start) > IWDG_UPDATE_CYCLES)) {
            return;
        }
    }
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
    wait_update(false, IWDG_FIRST_PASSES);
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
 * @brief Sets the firmware's time-out of about 2 s on the running IWDG, then reloads.
 *
 * Unlock key, prescaler /32, reload value 1999, bounded wait for the update, reload
 * [RM0440 Rev 9, Sec. 35.3.1, p. 1557]. The watchdog is already running (watchdog_start_long),
 * so no start key is written. If the update does not end in time the wait gives up and the
 * reload is written anyway, as before this change; the firmware then starts with whichever
 * time-out the chip has taken over.
 */
void watchdog_set_firmware_timeout(void)
{
    IWDG->KR = IWDG_KEY_UNLOCK;
    IWDG->PR = IWDG_PR_DIV32;
    IWDG->RLR = IWDG_RELOAD_FIRMWARE;
    wait_update(true, IWDG_UPDATE_PASSES);
    IWDG->KR = IWDG_KEY_RELOAD;
}
