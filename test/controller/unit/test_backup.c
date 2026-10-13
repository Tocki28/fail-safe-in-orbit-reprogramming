/*
 * test_backup.c — host unit tests of the bootloader's backup-register module
 * (src/controller/bootloader/backup.c), with RCC, PWR and TAMP replaced by RAM structures that
 * keep the real offsets (unit/backup_stubs).
 *
 * The rules under test (decision B, owner, 13 Oct 2026; common/backup_layout.h):
 *   - backup_open turns on the PWR and RTC APB clocks and sets DBP; backup_close clears DBP and the
 *     RTC APB clock;
 *   - pair n is TAMP_BKP(2n)R (value) and TAMP_BKP(2n+1)R (complement), at 0x100 + 4 x from the
 *     TAMP base;
 *   - a pair counts only when the second word is the complement of the first: all-zero registers
 *     (after an RTC domain reset) and every single flipped bit read as empty, never as a value;
 *   - a pair number outside the map is the fault reaction (common/fault.h), and nothing is
 *     written.
 * All of these fail on the old code: backup.c did not exist.
 *
 * Built with the Mac's cc and the undefined-behaviour sanitizer. Run: make -C test/controller
 * unit. Prints PASS or FAIL per test; exit status 1 on a failure.
 */
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backup.h"
#include "backup_layout.h"
#include "fault.h"
#include "stm32g4xx.h"

RCC_TypeDef fake_rcc;
PWR_TypeDef fake_pwr;
TAMP_TypeDef fake_tamp;

static uint32_t failures;
static uint32_t tests;
static jmp_buf fault_exit;
static bool fault_armed;
static volatile uint32_t fault_calls;
/* A value that no test writes, to see which registers were left alone. */
#define UNTOUCHED 0x5A5A5A5AU

/**
 * @brief Stand-in for the fault reaction: counts the call and leaves by longjmp to fault_exit.
 *
 * @param[in]     where  the function that found the impossible value
 * @note Does not return; stops the test program if no test expected a fault.
 */
void fault_impossible_value(const char *where)
{
    fault_calls++;
    if (fault_armed) {
        fault_armed = false;
        longjmp(fault_exit, 1);
    }
    printf("FAIL  fault reaction in %s, and no test expected it\n", where);
    exit(1);
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
 * @brief Clears the fake registers; every backup register holds UNTOUCHED.
 */
static void reset_rig(void)
{
    memset(&fake_rcc, 0, sizeof fake_rcc);
    memset(&fake_pwr, 0, sizeof fake_pwr);
    memset(&fake_tamp, 0, sizeof fake_tamp);
    for (uint32_t i = 0U; i < FAKE_TAMP_BKP_REGISTERS; i++) {
        fake_tamp.BKP[i] = UNTOUCHED;
    }
    fault_calls = 0U;
}

/**
 * @brief backup_open sets PWREN, RTCAPBEN and DBP; backup_close clears DBP and RTCAPBEN and
 * leaves PWREN for clock_back_to_reset.
 */
static void test_open_close(void)
{
    reset_rig();
    backup_open();
    report(((fake_rcc.APB1ENR1 & RCC_APB1ENR1_PWREN) != 0U) &&
               ((fake_rcc.APB1ENR1 & RCC_APB1ENR1_RTCAPBEN) != 0U) &&
               ((fake_pwr.CR1 & PWR_CR1_DBP) != 0U),
           "backup_open: PWR clock, RTC APB clock and DBP on");
    backup_close();
    report(((fake_pwr.CR1 & PWR_CR1_DBP) == 0U) &&
               ((fake_rcc.APB1ENR1 & RCC_APB1ENR1_RTCAPBEN) == 0U) &&
               ((fake_rcc.APB1ENR1 & RCC_APB1ENR1_PWREN) != 0U),
           "backup_close: DBP and RTC APB clock off; PWR clock left for clock_back_to_reset");
}

/**
 * @brief A written pair lands in TAMP_BKP(2n)R and TAMP_BKP(2n+1)R, reads back, and touches no
 * other register.
 */
static void test_write_read(void)
{
    uint32_t v = 0U;
    bool others = true;

    reset_rig();
    bool w0 = backup_write(BACKUP_PAIR_RESTARTS, 0x0102U);
    bool w4 = backup_write(BACKUP_PAIR_FALLBACK, 0x00010209U);
    for (uint32_t i = 0U; i < FAKE_TAMP_BKP_REGISTERS; i++) {
        if ((i != 0U) && (i != 1U) && (i != 8U) && (i != 9U)) {
            others = others && (fake_tamp.BKP[i] == UNTOUCHED);
        }
    }
    report(w0 && w4 && (fake_tamp.BKP[0] == 0x0102U) && (fake_tamp.BKP[1] == ~0x0102U) &&
               (fake_tamp.BKP[8] == 0x00010209U) && (fake_tamp.BKP[9] == ~0x00010209U) &&
               others,
           "backup_write: pair 0 -> BKP0R/BKP1R, pair 4 -> BKP8R/BKP9R (value, complement); "
           "no other register touched");
    report(backup_read(BACKUP_PAIR_RESTARTS, &v) && (v == 0x0102U) &&
               backup_read(BACKUP_PAIR_FALLBACK, &v) && (v == 0x00010209U),
           "backup_read: both pairs read back as written");
}

/**
 * @brief All-zero registers (an RTC domain reset) and every single flipped bit read as empty.
 */
static void test_broken_pairs(void)
{
    uint32_t v = 0xFFU;
    bool all_empty = true;
    uint32_t faked = 0U;

    reset_rig();
    memset((void *)fake_tamp.BKP, 0, sizeof fake_tamp.BKP);
    for (uint32_t pair = 0U; pair < BACKUP_PAIRS; pair++) {
        all_empty = all_empty && !backup_read(pair, &v) && (v == 0U);
    }
    report(all_empty, "all registers 0 (after an RTC domain reset): every pair reads as empty");

    for (uint32_t bit = 0U; bit < 64U; bit++) {
        reset_rig();
        (void)backup_write(BACKUP_PAIR_RESTARTS, 0x0203U);   /* the read below is the check */
        fake_tamp.BKP[bit / 32U] ^= (1UL << (bit % 32U));
        faked += backup_read(BACKUP_PAIR_RESTARTS, &v) ? 1U : 0U;
    }
    report(faked == 0U, "each of the 64 bits of a pair flipped in turn: read as empty every time "
                        "(one bit error cannot fake a count)");
}

/**
 * @brief A pair number outside the map is the fault reaction; nothing is read or written.
 */
static void test_pair_outside(void)
{
    uint32_t v = 0U;
    volatile bool returned = false;

    reset_rig();
    fault_armed = true;
    if (setjmp(fault_exit) == 0) {
        (void)backup_write(BACKUP_PAIRS, 1U);   /* must not come back */
        returned = true;
    }
    bool write_fault = !returned && (fault_calls == 1U);
    fault_armed = true;
    if (setjmp(fault_exit) == 0) {
        (void)backup_read(BACKUP_PAIRS, &v);   /* must not come back */
        returned = true;
    }
    bool untouched = true;
    for (uint32_t i = 0U; i < FAKE_TAMP_BKP_REGISTERS; i++) {
        untouched = untouched && (fake_tamp.BKP[i] == UNTOUCHED);
    }
    fault_armed = false;
    report(write_fault && !returned && (fault_calls == 2U) && untouched,
           "pair number 5 (outside the map): read and write -> fault reaction, no register "
           "touched");
}

/**
 * @brief Runs the tests and prints the totals.
 *
 * @return 0 if every test passed, 1 otherwise
 */
int main(void)
{
    test_open_close();
    test_write_read();
    test_broken_pairs();
    test_pair_outside();
    if (failures == 0U) {
        printf("backup register unit tests: all %u passed\n", tests);
        return 0;
    }
    printf("backup register unit tests: %u of %u FAILED\n", failures, tests);
    return 1;
}
