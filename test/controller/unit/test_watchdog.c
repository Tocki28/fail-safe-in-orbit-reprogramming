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
 * at the hand-over and waited on the cycle counter alone.
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

#define HISTORY_MAX 4096U
static uint32_t history[HISTORY_MAX];
static uint32_t history_length;

static uint32_t failures;
static uint32_t tests;

/* Stand-in for the cycle counter: 0 for ever, a counter that does not count. */
static uint32_t cycles_value;

/**
 * @brief Stand-in for the cycle counter read; returns a value the test sets.
 *
 * @return cycles_value
 */
uint32_t cycles_now(void)
{
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
    history_length = 0U;
    cycles_value = 0U;
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
    if (failures == 0U) {
        printf("watchdog unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("watchdog unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
