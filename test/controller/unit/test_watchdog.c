/*
 * test_watchdog.c — host unit tests of the bootloader's watchdog module (src/controller/
 * bootloader/watchdog.c), with the IWDG replaced by a RAM structure (unit/watchdog_stubs).
 *
 * The rules under test: watchdog_start_long writes the start key, the unlock key, then
 * prescaler /256 (PR = 6) and reload 0xFFF, and reloads (about 32 s); it ends even when the
 * status register never clears and the cycle counter is dead (the iteration backstop);
 * watchdog_reload writes 0xAAAA; watchdog_set_firmware_timeout sets prescaler /32 (PR = 3) and
 * reload 1999 (about 2 s) without the start key, then reloads, and also ends with a dead cycle
 * counter. Before decision A the bootloader's watchdog_start (in handover.c) was static, started
 * at the hand-over and waited on the cycle counter alone. watchdog_running (decision F(b)) is
 * true only with the IWDG status register 0 and LSIRDY set in RCC_CSR, and waits up to 50 ms for
 * the register to clear (an update takes up to five cycles of the prescaled clock, 43.4 ms at
 * the slowest LSI with /256); watchdog_set_firmware_timeout waits up to 10 ms (5.4 ms for /32).
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer. Run: make -C test/controller
 * unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <stdio.h>
#include <string.h>

#include "clock.h"
#include "stm32g4xx.h"
#include "watchdog.h"

IWDG_TypeDef fake_iwdg;
RCC_TypeDef fake_rcc;

#define HISTORY_MAX 4096U
static uint32_t history[HISTORY_MAX];
static uint32_t history_length;

static uint32_t failures;
static uint32_t tests;

/* Stand-in for the cycle counter: cycles_value moves by cycles_step at every read; a step of 0
 * is a counter that does not count. */
static uint32_t cycles_value;
static uint32_t cycles_step;
/* The status register clears when the counter reaches this value (0: never). It stands for the
 * hardware ending the update [RM0440 Rev 9, Sec. 35.4.4, p. 1562]. */
static uint32_t sr_clears_at;

/* 10 us of the 170 MHz core clock: the counter's step in the timed tests. */
#define STEP_10US 1700U
#define CYCLES_PER_MS (CORE_CLOCK_HZ / 1000U)

/**
 * @brief Stand-in for the clock in use.
 *
 * @return CORE_CLOCK_HZ
 */
uint32_t clock_hz(void)
{
    return CORE_CLOCK_HZ;
}

/**
 * @brief Stand-in for the cycle counter read: moves the counter on by cycles_step, clears the
 * status register when sr_clears_at is reached, returns the counter.
 *
 * @return cycles_value after the step
 */
uint32_t cycles_now(void)
{
    cycles_value += cycles_step;
    if ((sr_clears_at != 0U) && (cycles_value >= sr_clears_at)) {
        fake_iwdg.SR = 0U;
    }
    return cycles_value;
}

/**
 * @brief Records KR as it stands, then returns the fake IWDG.
 *
 * @return the fake IWDG registers
 */
IWDG_TypeDef *iwdg_access(void)
{
    if ((history_length == 0U) || (history[history_length - 1U] != fake_iwdg.KR)) {
        if (history_length < HISTORY_MAX) {
            history[history_length] = fake_iwdg.KR;
            history_length++;
        }
    }
    return &fake_iwdg;
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
 * @brief Clears the fake registers and the key history; the status register reads 0.
 */
static void reset_rig(void)
{
    memset(&fake_iwdg, 0, sizeof fake_iwdg);
    memset(&fake_rcc, 0, sizeof fake_rcc);
    history_length = 0U;
    cycles_value = 0U;
    cycles_step = 0U;
    sr_clears_at = 0U;
}

/**
 * @brief Returns true if the history holds the keys a, b, c in this order (not necessarily next
 * to each other).
 *
 * @param[in]     a  first key
 * @param[in]     b  second key
 * @param[in]     c  third key
 * @return true if a is followed later by b, and b later by c
 */
static bool keys_in_order(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t want[3] = {a, b, c};
    uint32_t next = 0U;

    for (uint32_t i = 0U; (i < history_length) && (next < 3U); i++) {
        if (history[i] == want[next]) {
            next++;
        }
    }
    return next == 3U;
}

/**
 * @brief Returns true if the history holds the key at least once.
 *
 * @param[in]     key  key value
 * @return true if some access saw KR equal to key
 */
static bool key_seen(uint32_t key)
{
    for (uint32_t i = 0U; i < history_length; i++) {
        if (history[i] == key) {
            return true;
        }
    }
    return false;
}

/**
 * @brief watchdog_start_long: keys in order, /256, reload 0xFFF, final reload.
 */
static void test_start_long(void)
{
    reset_rig();
    watchdog_start_long();
    (void)iwdg_access();   /* records the last key written */
    report(fake_iwdg.PR == 6U && fake_iwdg.RLR == 0xFFFU,
           "watchdog_start_long: PR = 6 (/256), RLR = 0xFFF (about 32 s)");
    report(keys_in_order(0xCCCCU, 0x5555U, 0xAAAAU),
           "watchdog_start_long: keys 0xCCCC, 0x5555, 0xAAAA in this order");
    report(fake_iwdg.KR == 0xAAAAU, "watchdog_start_long: ends with a reload (KR = 0xAAAA)");
}

/**
 * @brief watchdog_start_long ends although the status register never clears and no cycle
 * counter counts (the first call in main() is before the cycle counter is started).
 */
static void test_start_long_bounded(void)
{
    reset_rig();
    fake_iwdg.SR = 0x7U;   /* update never finishes: the LSI is not running */
    watchdog_start_long();
    report(fake_iwdg.PR == 6U, "watchdog_start_long: SR stuck, ends after its pass limit");
}

/**
 * @brief watchdog_reload writes 0xAAAA and nothing else.
 */
static void test_reload(void)
{
    reset_rig();
    fake_iwdg.PR = 1U;
    fake_iwdg.RLR = 2U;
    watchdog_reload();
    report((fake_iwdg.KR == 0xAAAAU) && (fake_iwdg.PR == 1U) && (fake_iwdg.RLR == 2U),
           "watchdog_reload: KR = 0xAAAA; prescaler and reload untouched");
}

/**
 * @brief watchdog_set_firmware_timeout: /32, reload 1999, no start key, reload last.
 */
static void test_firmware_timeout(void)
{
    reset_rig();
    fake_iwdg.PR = 6U;
    fake_iwdg.RLR = 0xFFFU;
    watchdog_set_firmware_timeout();
    (void)iwdg_access();
    report((fake_iwdg.PR == 3U) && (fake_iwdg.RLR == 1999U),
           "watchdog_set_firmware_timeout: PR = 3 (/32), RLR = 1999 (about 2 s)");
    report(key_seen(0x5555U) && (fake_iwdg.KR == 0xAAAAU),
           "watchdog_set_firmware_timeout: unlock key 0x5555, ends with a reload (KR = 0xAAAA)");
    report(!key_seen(0xCCCCU), "watchdog_set_firmware_timeout: no start key (already running)");
}

/**
 * @brief watchdog_set_firmware_timeout ends with the status register stuck and a dead cycle
 * counter: the pass limit is the only limit left.
 */
static void test_firmware_timeout_bounded(void)
{
    reset_rig();
    fake_iwdg.SR = 0x7U;
    cycles_value = 0U;
    watchdog_set_firmware_timeout();
    report(fake_iwdg.RLR == 1999U && fake_iwdg.KR == 0xAAAAU,
           "watchdog_set_firmware_timeout: SR stuck and cycle counter dead, ends and reloads");
}

/**
 * @brief watchdog_running (decision F(b)): true only with IWDG_SR = 0 and LSIRDY = 1; it hands
 * back the two raw values it read.
 *
 * Each of the three update bits (PVU, RVU, WVU [RM0440 Rev 9, Sec. 35.4.4, p. 1562]) left set, or
 * LSIRDY clear [RM0440 Rev 9, Sec. 7.4.28, p. 328], must answer false. Fails on the old code: the
 * function did not exist, and the bootloader started a trial without asking.
 */
static void test_running(void)
{
    const uint32_t update_bits[3] = {0x1U, 0x2U, 0x4U};
    bool any_true = false;

    uint32_t sr = 0xFFFFFFFFU;
    uint32_t csr = 0xFFFFFFFFU;
    uint32_t us = 0xFFFFFFFFU;

    reset_rig();
    fake_rcc.CSR = RCC_CSR_LSIRDY;
    report(watchdog_running(&sr, &csr, &us) && (sr == 0U) && (csr == RCC_CSR_LSIRDY),
           "watchdog_running: IWDG_SR = 0 and LSIRDY = 1 -> true, raw values handed back");
    for (uint32_t i = 0U; i < 3U; i++) {
        fake_iwdg.SR = update_bits[i];
        any_true = any_true || watchdog_running(&sr, &csr, &us);
    }
    report(!any_true && (sr == update_bits[2]) && (csr == RCC_CSR_LSIRDY),
           "watchdog_running: PVU, RVU or WVU set -> false, the raw values it decided on handed back");
    fake_iwdg.SR = 0U;
    fake_rcc.CSR = 0x0C000000U;   /* LSIRDY clear, other flags set */
    report(!watchdog_running(&sr, &csr, &us) && (sr == 0U) && (csr == 0x0C000000U),
           "watchdog_running: LSIRDY = 0 -> false (trial not started), raw values handed back");
    report(!watchdog_running(NULL, &csr, &us) && !watchdog_running(&sr, NULL, &us) &&
               !watchdog_running(&sr, &csr, NULL),
           "watchdog_running: a null pointer -> false, nothing waited");
}

/**
 * @brief watchdog_running: RVU still set several ms after the write, clearing within the bound
 * (the bench case, 13 Oct 2026) -> running, and the time waited is reported.
 *
 * Bench: IWDG_SR read 0x00000002 a few ms after the long time-out was written, 0 later. The
 * update ends at 43.4 ms in the worst case (5 x 256 / 29 500 [RM0440 Rev 9, Sec. 35.4.4, p. 1562;
 * DS12589 Rev 6, Table 44, p. 105]). Fails on the old code: it read IWDG_SR once and answered
 * false.
 */
static void test_running_settles_later(void)
{
    uint32_t sr = 0xFFFFFFFFU;
    uint32_t csr = 0U;
    uint32_t us = 0U;

    reset_rig();
    fake_rcc.CSR = RCC_CSR_LSIRDY;
    fake_iwdg.SR = 0x2U;
    cycles_step = STEP_10US;
    sr_clears_at = 43U * CYCLES_PER_MS;   /* 43 ms, the computed worst case */
    report(watchdog_running(&sr, &csr, &us) && (sr == 0U) && (csr == RCC_CSR_LSIRDY),
           "watchdog_running: RVU set, clears at 43 ms -> running");
    report((us >= 42980U) && (us <= 43020U),
           "watchdog_running: reports the time waited (43 ms, to 10 us)");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_LSIRDY;
    fake_iwdg.SR = 0x2U;
    cycles_step = STEP_10US;
    sr_clears_at = 49U * CYCLES_PER_MS;
    report(watchdog_running(&sr, &csr, &us) && (us >= 48980U) && (us <= 49020U),
           "watchdog_running: RVU clears at 49 ms, just inside the 50 ms bound -> running");
}

/**
 * @brief watchdog_running: RVU never clearing -> false after the 50 ms bound, with the raw value
 * and the time waited handed back; also false when it clears only after the bound.
 */
static void test_running_never_settles(void)
{
    uint32_t sr = 0U;
    uint32_t csr = 0U;
    uint32_t us = 0U;

    reset_rig();
    fake_rcc.CSR = RCC_CSR_LSIRDY;
    fake_iwdg.SR = 0x2U;
    cycles_step = STEP_10US;
    report(!watchdog_running(&sr, &csr, &us) && (sr == 0x2U) && (csr == RCC_CSR_LSIRDY),
           "watchdog_running: RVU never clears -> false, raw values handed back");
    report((us >= 50000U) && (us <= 50020U),
           "watchdog_running: gave up after 50 ms (reported to 10 us)");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_LSIRDY;
    fake_iwdg.SR = 0x2U;
    cycles_step = STEP_10US;
    sr_clears_at = 60U * CYCLES_PER_MS;
    report(!watchdog_running(&sr, &csr, &us),
           "watchdog_running: RVU clears at 60 ms, after the bound -> false");

    reset_rig();
    fake_rcc.CSR = RCC_CSR_LSIRDY;
    fake_iwdg.SR = 0x2U;
    cycles_step = 0U;   /* a dead cycle counter: only the pass limit ends the wait */
    report(!watchdog_running(&sr, &csr, &us) && (us == 0U),
           "watchdog_running: RVU stuck, cycle counter dead -> ends on the pass limit, false");
}

/**
 * @brief watchdog_running: LSIRDY clear -> false even when IWDG_SR is 0, without waiting.
 */
static void test_running_lsi_not_ready(void)
{
    uint32_t sr = 0xFFFFFFFFU;
    uint32_t csr = 0xFFFFFFFFU;
    uint32_t us = 0xFFFFFFFFU;

    reset_rig();
    fake_rcc.CSR = 0U;
    cycles_step = STEP_10US;
    report(!watchdog_running(&sr, &csr, &us) && (sr == 0U) && (csr == 0U) && (us < 100U),
           "watchdog_running: LSIRDY = 0, IWDG_SR = 0 -> false, no wait");
}

/**
 * @brief watchdog_set_firmware_timeout waits for an update that takes the computed worst case.
 *
 * The update of prescaler /32 takes up to 5 x 32 / 29 500 = 5.4 ms [RM0440 Rev 9, Sec. 35.4.4,
 * p. 1562; DS12589 Rev 6, Table 44, p. 105]. The status register clears at 5.5 ms; the wait must
 * not have ended before. Fails on the old code: its bound was 2 ms. Then, with the register
 * stuck, it ends at 10 ms.
 */
static void test_firmware_timeout_waits_for_update(void)
{
    reset_rig();
    fake_iwdg.SR = 0x3U;
    cycles_step = STEP_10US;
    sr_clears_at = 5U * CYCLES_PER_MS + CYCLES_PER_MS / 2U;
    watchdog_set_firmware_timeout();
    report((fake_iwdg.SR == 0U) && (fake_iwdg.KR == 0xAAAAU),
           "watchdog_set_firmware_timeout: update ending at 5.5 ms is waited for, then reload");

    reset_rig();
    fake_iwdg.SR = 0x3U;
    cycles_step = STEP_10US;
    watchdog_set_firmware_timeout();
    report((fake_iwdg.SR != 0U) && (fake_iwdg.KR == 0xAAAAU) &&
               (cycles_value >= 10U * CYCLES_PER_MS) &&
               (cycles_value <= 10U * CYCLES_PER_MS + 2U * STEP_10US),
           "watchdog_set_firmware_timeout: update never ends -> gives up at 10 ms, reloads");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    test_start_long();
    test_start_long_bounded();
    test_reload();
    test_firmware_timeout();
    test_firmware_timeout_bounded();
    test_running();
    test_running_settles_later();
    test_running_never_settles();
    test_running_lsi_not_ready();
    test_firmware_timeout_waits_for_update();
    if (failures == 0U) {
        printf("watchdog unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("watchdog unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
