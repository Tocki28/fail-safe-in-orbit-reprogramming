/*
 * core_clock.cpp — core clock up to 170 MHz, cycle counter, millisecond tick; register level.
 *
 * The clock steps are the bootloader's (bootloader/clock.c), copied, not shared: REQ-119 asks
 * the bootloader to share no code with the main firmware. Two differences: every wait here
 * gives up after kWaitCycles cycles or kWaitPasses passes, and there is no way back to the
 * reset clock (the firmware hands over to nothing).
 *
 * The firmware normally starts on HSI16 at 16 MHz, 1 flash wait state: the bootloader puts the
 * clock back to its reset state before the jump (bootloader/handover.h). That is not always so:
 * if the bootloader's switch back to HSI16 does not finish, it returns early and the firmware
 * starts on the PLL (bootloader/clock.c, clock_back_to_reset). start() copes with both: it
 * writes every clock setting it needs, in the same order, whichever clock it finds.
 *
 * Decision C (owner, 13 Oct 2026): if the PLL does not start, the firmware runs on HSI16 at
 * 16 MHz instead of spinning until the watchdog restarts it; start() returns the clock in use, and
 * the UART divider and the SysTick reload are computed from it (main.cpp). Before, start() left
 * the clock as it was at the failed step and main() stopped reloading the watchdog.
 */
#include "core_clock.hpp"

#include "fault.hpp"
#include "stm32g4xx.h"

/* C++: an unnamed namespace instead of `static` on each name: everything inside is private to
 * this file, as the C file's static functions and #defines were. */
namespace {

/* PLL from HSI16: / M = 4 MHz into the VCO (2.66-16 MHz allowed), x N = 340 MHz VCO
 * (96-344 MHz allowed), / R = 170 MHz [DS12589 Rev 6, Table 45, p. 106]. */
constexpr uint32_t kPllM = 4U;
constexpr uint32_t kPllN = 85U;
/* PLLM field holds M - 1; PLLR field 00 means divide by 2 [RM0440 Rev 9, Sec. 7.4.4, p. 292-293]. */
constexpr uint32_t kPllmField = kPllM - 1U;
constexpr uint32_t kPllrDiv2Field = 0U;
/* Boost-mode entry waits at least 1 us before the AHB divider goes back to 1
 * [RM0440 Rev 9, Sec. 6.1.5, p. 233]; 170 cycles is >= 1 us at any clock up to 170 MHz. */
constexpr uint32_t kSettleCycles = kCoreClockHz / 1000000U;
/* Longest wait here: PLL lock, 40 us at most [DS12589 Rev 6, Table 45, p. 106]. 16 000 cycles
 * last at least 94 us (at 170 MHz), over twice that, and at most 2 ms (at the 8 MHz the core
 * runs at during the change), far inside the watchdog's 1.88 s (bootloader/watchdog.c). */
constexpr uint32_t kWaitCycles = 16000U;
/* Backstop for a cycle counter that has stopped: as many passes of the wait loop as cycles
 * above. A pass reads a register and the counter, so it takes at least one core cycle:
 * (1) with a working counter kWaitCycles cycles pass in at most kWaitCycles passes, so the
 * cycle limit ends the wait first and the backstop never changes a result; (2) with a dead
 * counter kWaitCycles passes last at least kWaitCycles cycles, which is at least 94 us at
 * 170 MHz and 1 ms at 16 MHz, over the 40 us PLL lock above. The longest a dead-counter wait
 * can take is kWaitCycles x (cycles per pass): TBD, not measured; it stays inside the
 * watchdog's 1.88 s as long as a pass takes under 1.88 s x 16 MHz / 16 000 = 1880 cycles
 * (computed at the 16 MHz reset clock; the bootloader's time-out is
 * bootloader/watchdog.c, IWDG_PR_DIV32, IWDG_RELOAD_FIRMWARE). */
constexpr uint32_t kWaitPasses = kWaitCycles;
/* After the counter is enabled it must change within this many reads. A counter that counts
 * core cycles changes between two reads at least one cycle apart; 100 reads span at least 100
 * cycles. No count rate is assumed beyond "changes at all". */
constexpr uint32_t kCycleCheckReads = 100U;

/**
 * @brief Enable and zero the DWT cycle counter, then check that it counts.
 *
 * The DWT cycle counter runs only with TRCENA set in DEMCR [RM0440 Rev 9, Sec. 47.10,
 * p. 2102] and CYCCNTENA (bit 0) set in DWT_CTRL [RM0440 Rev 9, Sec. 47.13, p. 2104]. Every
 * wait in this file and in data-link/uart_link.cpp is bounded by this counter, so start()
 * reports a counter that does not change within kCycleCheckReads reads.
 *
 * @return true if the counter advanced, false if it did not
 */
bool cyclesStart()
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    const uint32_t first = DWT->CYCCNT;
    for (uint32_t n = 0U; n < kCycleCheckReads; n++) {
        if (DWT->CYCCNT != first) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Wait until (reg & mask) == want, at most kWaitCycles cycles or kWaitPasses passes.
 *
 * C++: the register is passed by reference, not by pointer: it always exists, so there is no
 * null case to think about. The timeout is measured on the cycle counter; the passes limit is
 * the backstop for a stopped counter (see kWaitPasses).
 *
 * @param[in] reg   the register to poll (read each pass)
 * @param[in] mask  the bits to look at
 * @param[in] want  the value those bits must have
 * @return true if the bits reached the wanted value; false if the limit passed first
 */
bool waitBits(const volatile uint32_t &reg, uint32_t mask, uint32_t want)
{
    uint32_t start = CoreClock::cyclesNow();

    /* Unsigned subtraction stays right when the counter wraps. */
    for (uint32_t n = 0U; n < kWaitPasses; n++) {
        if ((reg & mask) == want) {
            return true;
        }
        if ((CoreClock::cyclesNow() - start) > kWaitCycles) {
            return false;
        }
    }
    return (reg & mask) == want;
}

/**
 * @brief Busy-wait for a number of core clock cycles, at most that many passes of the loop.
 *
 * Uses the cycle counter; unsigned subtraction keeps the result right when the counter wraps.
 * The passes limit is the backstop for a stopped counter: every pass takes at least one core
 * cycle, so `cycles` passes last at least `cycles` cycles, and with a working counter the
 * cycle count ends the wait first (see kWaitPasses).
 *
 * @param[in] cycles  cycles to wait (core clock cycles)
 */
void waitCycles(uint32_t cycles)
{
    uint32_t start = CoreClock::cyclesNow();

    for (uint32_t n = 0U; (n < cycles) && ((CoreClock::cyclesNow() - start) < cycles); n++) {
    }
}

/**
 * @brief Select the system clock source and wait until the chip shows it in use.
 *
 * @param[in] sw   value for RCC_CFGR SW
 * @param[in] sws  value RCC_CFGR SWS must show
 * @return true if SWS showed the source in time; false if the wait timed out
 */
bool selectSystemClock(uint32_t sw, uint32_t sws)
{
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | sw;
    return waitBits(RCC->CFGR, RCC_CFGR_SWS_Msk, sws);
}

/**
 * @brief After a step towards 170 MHz failed: HSI16 as the system clock, PLL off, AHB / 1
 * (decision C).
 *
 * The same fall-back as the bootloader's (bootloader/clock.c, stay_on_hsi16), copied, not shared
 * (REQ-119). The 4 wait states and boost mode may stay: both are allowed at 16 MHz [RM0440 Rev 9,
 * Table 29, p. 190; Sec. 6.1.5, p. 233]. If even the switch to HSI16 does not finish, the source
 * is not known and nothing is changed further: 0, and main() treats the clock as not usable.
 *
 * @return kHsi16Hz if the core now runs from HSI16, 0 if the switch did not finish
 */
uint32_t stayOnHsi16()
{
    if (!selectSystemClock(RCC_CFGR_SW_HSI, RCC_CFGR_SWS_HSI)) {
        return 0U;
    }
    RCC->CR &= ~RCC_CR_PLLON;
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;   /* AHB / 1: the core at the full 16 MHz */
    return kHsi16Hz;
}

/**
 * @brief Set the flash wait states and wait until the new value is in use.
 *
 * @param[in] latency  the FLASH_ACR_LATENCY_xWS value to write
 * @return true if the read-back showed the new value in time; false if the wait timed out
 */
bool setFlashLatency(uint32_t latency)
{
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) | latency;
    /* Read back until the new value is in use [RM0440 Rev 9, Sec. 5.3.3, p. 191, step 2]. */
    return waitBits(FLASH->ACR, FLASH_ACR_LATENCY_Msk, latency);
}

}  // namespace

/**
 * @brief Read the DWT cycle counter.
 *
 * @return the counter value, in core clock cycles; wraps at 2^32
 */
uint32_t CoreClock::cyclesNow()
{
    return DWT->CYCCNT;
}

/**
 * @brief Start the cycle counter and move the core clock from HSI16 to the 170 MHz PLL.
 *
 * Steps: AHB / 2, boost mode, 4 flash wait states with prefetch, PLL on and selected as system
 * clock, 1 us wait, AHB / 1. Every wait gives up after kWaitCycles cycles or kWaitPasses passes.
 * When a step does not finish, the core goes back to HSI16 (stayOnHsi16) and the firmware runs
 * at 16 MHz (decision C); main.cpp reports it with EVENT 0x0001 (mode-manager/mode_manager.cpp).
 *
 * A cycle counter that does not count is handled as no usable clock: every wait here and in the
 * link driver depends on it, so 0 is returned and nothing else is touched (main.cpp then stops
 * reloading the watchdog, which restarts the chip, REQ-022; the bootloader counts that restart,
 * decision B). The firmware has no log line before its link is up.
 *
 * @return kCoreClockHz if the core now runs at 170 MHz; kHsi16Hz if it went back to HSI16; 0 if
 *         the cycle counter does not count or the switch back to HSI16 did not finish
 */
uint32_t CoreClock::start()
{
    if (!cyclesStart()) {
        return 0U;
    }

    /* PWR registers ignore writes until their bus clock is on. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;   /* read back, value not needed: the clock runs before PWR is written */

    /* Range 1 normal -> boost mode, steps 1-5 [RM0440 Rev 9, Sec. 6.1.5, p. 233];
     * above 150 MHz boost mode is needed [RM0440 Rev 9, Table 38, p. 232]. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_HPRE_Msk) | RCC_CFGR_HPRE_DIV2;   /* 1: AHB / 2 */
    PWR->CR5 &= ~PWR_CR5_R1MODE;                                        /* 2: boost mode */
    /* 3: 4 wait states for HCLK <= 170 MHz in boost mode [RM0440 Rev 9, Table 29, p. 190];
     * prefetch on, to hide most of those wait states [RM0440 Rev 9, Sec. 5.3.4, p. 192]. */
    FLASH->ACR |= FLASH_ACR_PRFTEN;
    if (!setFlashLatency(FLASH_ACR_LATENCY_4WS)) {
        return stayOnHsi16();
    }

    /* 4: PLL on, then make it the system clock. Fields can change only while it is off. */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | (kPllmField << RCC_PLLCFGR_PLLM_Pos) |
                   (kPllN << RCC_PLLCFGR_PLLN_Pos) | (kPllrDiv2Field << RCC_PLLCFGR_PLLR_Pos) |
                   RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    if (!waitBits(RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY) ||
        !selectSystemClock(RCC_CFGR_SW_PLL, RCC_CFGR_SWS_PLL)) {
        return stayOnHsi16();
    }

    waitCycles(kSettleCycles);                                          /* 5: wait 1 us, */
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;                                    /*    then AHB / 1 */
    return kCoreClockHz;
}

/**
 * @brief Start SysTick as a 1 ms interrupt from the processor clock in use.
 *
 * SysTick counts processor clocks down from RELOAD to 0 [Arm DUI 0553A, Sec. 4.4.1,
 * p. 4-33 to 4-34], so RELOAD = core_hz / 1000 - 1: 169 999 at 170 MHz, 15 999 at 16 MHz after
 * a clock fault (decision C: computed from the clock actually in use, so a millisecond stays a
 * millisecond); both fit its 24 bits (SysTick_LOAD_RELOAD_Msk, core_cm4.h). core_hz can only be
 * one of the two clocks start() returns; any other value is a corrupted variable: the fault
 * reaction (firmware/fault.hpp). No interrupt priority is set: this handler and USART2's are a
 * few instructions each, so neither delays the other by more than a fraction of one byte time
 * (86.8 us).
 *
 * @param[in] core_hz  the core clock in use, kCoreClockHz or kHsi16Hz (CoreClock::start)
 * @note Does not return for any other core_hz.
 */
void CoreClock::startTick(uint32_t core_hz)
{
    if ((core_hz != kCoreClockHz) && (core_hz != kHsi16Hz)) {
        faultImpossibleValue("CoreClock::startTick");
    }
    SysTick->LOAD = (core_hz / kMsPerSecond) - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

/**
 * @brief Add one millisecond to the tick count; called from the SysTick interrupt.
 */
void CoreClock::onTick()
{
    /* C++17 allows ++ on a volatile (C++20 deprecates it); written out, the read and the write
     * are visible. Only this interrupt writes the count, so nothing can come between them. */
    milliseconds_ = milliseconds_ + 1U;
}

/**
 * @brief Read the millisecond tick count.
 *
 * @return milliseconds since startTick(); wraps at 2^32
 */
uint32_t CoreClock::tickMs() const
{
    /* One aligned 32-bit read: the interrupt cannot split it. */
    return milliseconds_;
}
