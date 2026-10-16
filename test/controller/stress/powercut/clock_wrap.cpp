/*
 * clock_wrap.cpp — host stress test of the controller clock at its counter wraps (stress audit,
 * Part A.4, controller half): the millisecond tick count of CoreClock and the 32-bit cycle
 * counter that bounds every wait.
 *
 * What it is: a program that drives the real firmware/core_clock.cpp against the unit tests' RAM
 * register stand-ins (test/controller/unit/board_stubs/stm32g4xx.h, used read-only). Two checks:
 *   1. the millisecond count: onTick() is called 2^32 - 1 times, so tickMs() reads 0xFFFFFFFF,
 *      then once more (it must read 0, then 1). The count is private and only onTick() changes
 *      it, so the only way to reach the wrap through the real interface is to tick that often
 *      (about 4.3 * 10^9 calls; one millisecond each is 49.7 days on the board, core_clock.hpp);
 *   2. the cycle counter (DWT CYCCNT, wraps every 25 s at 170 MHz, core_clock.hpp): start() runs
 *      its bounded waits (waitBits, waitCycles) while the counter wraps at many points inside
 *      the wait, with a PLL that locks (start() must return kCoreClockHz) and with one that never
 *      locks (start() must give up and return kHsi16Hz, within the wait's bound). The stand-in
 *      counter is set near 0xFFFFFFFF at a chosen read, which the board_stubs model allows
 *      (FakeCycleCounter::value is a public field of the stand-in, not of the flight code).
 *
 * Why: every wait of the firmware uses "now - start > limit" in unsigned arithmetic. That is right
 * across a wrap only if no wait compares the counter itself (core_clock.cpp comments, waitBits).
 *
 * Built with the Mac's clang++, the board's warning set, the undefined-behaviour sanitizer and
 * libc++'s debug hardening (rule E5). Exit status 1 if a check failed.
 */
#include <cstdint>
#include <cstdio>

#include "core_clock.hpp"
#include "fake_fault.hpp"
#include "stm32g4xx.h"

FakeChip fake_chip;
DWT_Type fake_dwt;
CoreDebug_Type fake_coredebug;
RCC_TypeDef fake_rcc;
PWR_TypeDef fake_pwr;
FLASH_TypeDef fake_flash_regs;
SysTick_Type fake_systick;

namespace {

constexpr uint32_t kMax32 = 0xFFFFFFFFU;
/* The cycle counter is read twice by cyclesStart() (core_clock.cpp: the first value, then one
 * read that must differ). The counter is moved at the next read, so the jump lands at the start
 * of the first wait (setFlashLatency). */
constexpr uint32_t kJumpAtRead = 2U;
/* kWaitCycles and kWaitPasses of core_clock.cpp (16 000 each), repeated here only to choose
 * wrap points around them: below, at and above the longest wait. */
constexpr uint32_t kLongWait = 16000U;
/* Most counter reads one start() may make: the PLL wait, the switch wait and the flash wait can
 * each take kLongWait passes, plus the settle wait of 170 cycles (kSettleCycles), plus the
 * three start reads and some slack. A run that reads more is not bounded as the code says. */
constexpr uint32_t kMaxReads = (3U * kLongWait) + 1000U;

uint32_t g_failures;
uint32_t g_checks;
uint32_t g_reads;        /* counter reads since reset */
uint32_t g_jump_to;      /* value the counter is set to at the jump read */
uint32_t g_final;        /* the counter's value when the last start() returned */

}  // namespace

/**
 * @brief One step of the modelled chip (board_stubs/stm32g4xx.h), plus this test's counter jump.
 *
 * PLLRDY follows PLLON when the PLL locks; SWS follows SW when the switch works; at read number
 * kJumpAtRead the cycle counter is set to g_jump_to.
 */
void fakeChipStep()
{
    if (g_reads == kJumpAtRead) {
        fake_dwt.CYCCNT.value = g_jump_to;
    }
    g_reads++;
    if ((fake_rcc.CR & RCC_CR_PLLON) == 0U) {
        fake_rcc.CR &= ~RCC_CR_PLLRDY;
    } else if (fake_chip.pll_locks) {
        fake_rcc.CR |= RCC_CR_PLLRDY;
    }
    if (fake_chip.sws_follows) {
        fake_rcc.CFGR =
            (fake_rcc.CFGR & ~RCC_CFGR_SWS_Msk) | ((fake_rcc.CFGR & RCC_CFGR_SW_Msk) << 2U);
    }
}

namespace {

/**
 * @brief Counts and prints one check.
 *
 * @param[in]     ok    true if the check passed
 * @param[in]     name  what was checked
 */
void report(bool ok, const char *name)
{
    g_checks++;
    if (!ok) {
        g_failures++;
    }
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
}

/**
 * @brief Puts the registers as after a reset: HSI16 selected and in use, PLL off.
 *
 * @param[in]     pll_locks  true if the PLL locks, false if PLLRDY never comes
 * @param[in]     jump_to    value the cycle counter jumps to at the jump read
 */
void resetChip(bool pll_locks, uint32_t jump_to)
{
    fake_chip = FakeChip{true, pll_locks, true, 0U, 0U};
    fake_dwt = DWT_Type{};
    fake_coredebug = CoreDebug_Type{};
    fake_rcc = RCC_TypeDef{};
    fake_rcc.CFGR = RCC_CFGR_SW_HSI | RCC_CFGR_SWS_HSI;
    fake_pwr = PWR_TypeDef{};
    fake_flash_regs = FLASH_TypeDef{};
    fake_systick = SysTick_Type{};
    g_reads = 0U;
    g_jump_to = jump_to;
    faultClear();
}

/**
 * @brief The millisecond count across its wrap: 2^32 - 1 ticks, then two more.
 *
 * Prints the three readings and the elapsed time a caller computes with unsigned subtraction
 * (mode_manager.cpp, arm_state.cpp), which must be 2 across the wrap.
 */
void tickWrap()
{
    CoreClock clock;
    uint32_t atMax = 0U;
    uint32_t atZero = 0U;
    uint32_t atOne = 0U;

    for (uint32_t i = 0U; i < kMax32; i++) {
        clock.onTick();
    }
    atMax = clock.tickMs();
    clock.onTick();
    atZero = clock.tickMs();
    clock.onTick();
    atOne = clock.tickMs();
    std::printf("  tickMs after 2^32 - 1 ticks: 0x%08X, after one more: %u, after one more: %u; "
                "elapsed since the 0xFFFFFFFF reading (unsigned subtraction): %u ms\n", atMax,
                atZero, atOne, atOne - atMax);
    report((atMax == kMax32) && (atZero == 0U) && (atOne == 1U) && ((atOne - atMax) == 2U),
           "CoreClock: the millisecond count reads 0xFFFFFFFF, then wraps to 0, then 1, and "
           "elapsed time across the wrap is right");
}

/**
 * @brief start() with the cycle counter wrapping at `k` reads after the jump, with a PLL that
 * locks and with one that never locks.
 *
 * @param[in]     k        the counter jumps to 0xFFFFFFFF - k + 1, so it wraps after k reads
 * @param[out]    ok_lock  true if start() returned kCoreClockHz with the locking PLL
 * @param[out]    ok_fail  true if start() gave up with kHsi16Hz within kMaxReads
 * @param[out]    reads    counter reads the failing run made
 */
void cycleWrapAt(uint32_t k, bool *ok_lock, bool *ok_fail, uint32_t *reads)
{
    resetChip(true, kMax32 - k + 1U);
    *ok_lock = (CoreClock::start() == kCoreClockHz);
    resetChip(false, kMax32 - k + 1U);
    *ok_fail = (CoreClock::start() == kHsi16Hz) && (g_reads <= kMaxReads);
    *reads = g_reads;
    g_final = fake_dwt.CYCCNT.value;
}

/**
 * @brief The cycle counter across its wrap inside start()'s bounded waits.
 *
 * Wrap points: the first reads of every wait (0 to 5), inside the 170-cycle settle wait (10, 100,
 * 150, 171, 172), and inside the PLL wait that never ends well (500, 8000, 15 999, 16 000,
 * 16 001, 16 002, 20 000).
 */
void cycleWrap()
{
    const uint32_t points[] = {0U,   1U,   2U,    3U,    4U,     5U,     10U,
                               100U, 150U, 171U,  172U,  500U,   8000U,  15999U,
                               16000U, 16001U, 16002U, 20000U};
    bool allLock = true;
    bool allFail = true;
    uint32_t maxReads = 0U;

    for (uint32_t k : points) {
        bool lock = false;
        bool fail = false;
        uint32_t reads = 0U;

        cycleWrapAt(k, &lock, &fail, &reads);
        allLock = allLock && lock;
        allFail = allFail && fail;
        maxReads = (reads > maxReads) ? reads : maxReads;
        std::printf("  counter wraps %5u reads after the jump: PLL locks -> %s; PLL never locks "
                    "-> %s (%u counter reads, counter ends at 0x%08X)\n", k,
                    lock ? "170 MHz" : "WRONG", fail ? "gave up, HSI16" : "WRONG", reads, g_final);
    }
    report(allLock, "CoreClock::start(): the bounded waits end right when the cycle counter wraps "
                    "inside them (PLL locks: 170 MHz)");
    report(allFail, "CoreClock::start(): a wait that must time out ends within its bound when the "
                    "counter wraps inside it (PLL never locks: HSI16)");
    std::printf("  most counter reads in one start() with a wait that times out: %u (bound used: "
                "%u)\n", maxReads, kMaxReads);
}

}  // namespace

/**
 * @brief Runs both clock checks.
 *
 * @return 0 if every check passed, 1 otherwise
 */
int main()
{
    cycleWrap();
    tickWrap();
    std::printf("%s  %u checks, %u failed\n", (g_failures == 0U) ? "PASS" : "FAIL", g_checks,
                g_failures);
    return (g_failures == 0U) ? 0 : 1;
}
