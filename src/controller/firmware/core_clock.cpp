/*
 * core_clock.cpp — core clock up to 170 MHz, cycle counter, millisecond tick; register level.
 *
 * The clock steps are the bootloader's (bootloader/clock.c), copied, not shared: REQ-119 asks
 * the bootloader to share no code with the main firmware. Two differences: every wait here
 * gives up after kWaitCycles, and there is no way back to the reset clock (the firmware hands
 * over to nothing).
 *
 * The firmware starts on HSI16 at 16 MHz, 1 flash wait state: the bootloader puts the clock
 * back to its reset state before the jump (bootloader/handover.h).
 */
#include "core_clock.hpp"

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
 * runs at during the change), far inside the watchdog's 1.88 s (bootloader/handover.c). */
constexpr uint32_t kWaitCycles = 16000U;

void cyclesStart()
{
    /* The DWT cycle counter runs only with TRCENA set in DEMCR [RM0440 Rev 9, Sec. 47.10,
     * p. 2102] and CYCCNTENA (bit 0) set in DWT_CTRL [RM0440 Rev 9, Sec. 47.13, p. 2104]. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* Wait until (reg & mask) == want, at most kWaitCycles. C++: the register is passed by
 * reference, not by pointer: it always exists, so there is no null case to think about. */
bool waitBits(const volatile uint32_t &reg, uint32_t mask, uint32_t want)
{
    uint32_t start = CoreClock::cyclesNow();

    /* Unsigned subtraction stays right when the counter wraps. */
    while ((reg & mask) != want) {
        if ((CoreClock::cyclesNow() - start) > kWaitCycles) {
            return false;
        }
    }
    return true;
}

void waitCycles(uint32_t cycles)
{
    uint32_t start = CoreClock::cyclesNow();

    while ((CoreClock::cyclesNow() - start) < cycles) {
    }
}

bool setFlashLatency(uint32_t latency)
{
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) | latency;
    /* Read back until the new value is in use [RM0440 Rev 9, Sec. 5.3.3, p. 191, step 2]. */
    return waitBits(FLASH->ACR, FLASH_ACR_LATENCY_Msk, latency);
}

}  // namespace

uint32_t CoreClock::cyclesNow()
{
    return DWT->CYCCNT;
}

bool CoreClock::start()
{
    cyclesStart();

    /* PWR registers ignore writes until their bus clock is on. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;

    /* Range 1 normal -> boost mode, steps 1-5 [RM0440 Rev 9, Sec. 6.1.5, p. 233];
     * above 150 MHz boost mode is needed [RM0440 Rev 9, Table 38, p. 232]. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_HPRE_Msk) | RCC_CFGR_HPRE_DIV2;   /* 1: AHB / 2 */
    PWR->CR5 &= ~PWR_CR5_R1MODE;                                        /* 2: boost mode */
    /* 3: 4 wait states for HCLK <= 170 MHz in boost mode [RM0440 Rev 9, Table 29, p. 190];
     * prefetch on, to hide most of those wait states [RM0440 Rev 9, Sec. 5.3.4, p. 192]. */
    FLASH->ACR |= FLASH_ACR_PRFTEN;
    if (!setFlashLatency(FLASH_ACR_LATENCY_4WS)) {
        return false;
    }

    /* 4: PLL on, then make it the system clock. Fields can change only while it is off. */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | (kPllmField << RCC_PLLCFGR_PLLM_Pos) |
                   (kPllN << RCC_PLLCFGR_PLLN_Pos) | (kPllrDiv2Field << RCC_PLLCFGR_PLLR_Pos) |
                   RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    if (!waitBits(RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY)) {
        return false;
    }
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | RCC_CFGR_SW_PLL;
    if (!waitBits(RCC->CFGR, RCC_CFGR_SWS_Msk, RCC_CFGR_SWS_PLL)) {
        return false;
    }

    waitCycles(kSettleCycles);                                          /* 5: wait 1 us, */
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;                                    /*    then AHB / 1 */
    return true;
}

/* SysTick counts processor clocks down from RELOAD to 0 [Arm DUI 0553A, Sec. 4.4.1,
 * p. 4-33 to 4-34]; RELOAD = 169 999 fits its 24 bits (SysTick_LOAD_RELOAD_Msk, core_cm4.h).
 * No interrupt priority is set: this handler and USART2's are a few instructions each, so
 * neither delays the other by more than a fraction of one byte time (86.8 us). */
void CoreClock::startTick()
{
    SysTick->LOAD = kCyclesPerMs - 1U;
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

void CoreClock::onTick()
{
    /* C++17 allows ++ on a volatile (C++20 deprecates it); written out, the read and the write
     * are visible. Only this interrupt writes the count, so nothing can come between them. */
    milliseconds_ = milliseconds_ + 1U;
}

uint32_t CoreClock::tickMs() const
{
    /* One aligned 32-bit read: the interrupt cannot split it. */
    return milliseconds_;
}
