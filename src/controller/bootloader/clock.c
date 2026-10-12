/*
 * clock.c — core clock up to 170 MHz and back, register level.
 * After reset the core runs from HSI16 at 16 MHz with 1 flash wait state
 * [RM0440 Rev 9, Sec. 5.3.3, p. 190].
 *
 * Every wait gives up after WAIT_CYCLES: the bootloader runs before any watchdog is started, so
 * a wait that never ends would stop the chip for good. If the PLL does not lock, the core stays
 * on HSI16 and the bootloader carries on, slower.
 */
#include "clock.h"

#include "stm32g4xx.h"

/* PLL from HSI16: / M = 4 MHz into the VCO (2.66-16 MHz allowed), x N = 340 MHz VCO
 * (96-344 MHz allowed), / R = 170 MHz [DS12589 Rev 6, Table 45, p. 106]. */
#define PLL_M                4U
#define PLL_N                85U
/* PLLM field holds M - 1; PLLR field 00 means divide by 2 [RM0440 Rev 9, Sec. 7.4.4, p. 292-293]. */
#define PLLM_FIELD           (PLL_M - 1U)
#define PLLR_DIV2_FIELD      0U
/* RCC_PLLCFGR value after reset [RM0440 Rev 9, Sec. 7.4.4, p. 291]. */
#define PLLCFGR_RESET_VALUE  0x00001000U
/* Boost-mode entry waits at least 1 us before the AHB divider goes back to 1
 * [RM0440 Rev 9, Sec. 6.1.5, p. 233]; 170 cycles is >= 1 us at any clock up to 170 MHz. */
#define SETTLE_CYCLES        CYCLES_PER_US
/* Longest wait here: PLL lock, 40 us at most [DS12589 Rev 6, Table 45, p. 106]. 16 000 cycles
 * last at least 94 us (at 170 MHz), over twice that, and at most 2 ms (at the 8 MHz the core
 * runs at during the change). Same bound as the firmware's copy (firmware/core_clock.c). */
#define WAIT_CYCLES          16000U

/* The clock in use, for the UART divider and the timing log. 0 = not known: a switch of the
 * clock source did not finish. */
static uint32_t core_hz = HSI16_HZ;

void cycles_start(void)
{
    /* The DWT cycle counter runs only with TRCENA set in DEMCR [RM0440 Rev 9, Sec. 47.10,
     * p. 2102] and CYCCNTENA (bit 0) set in DWT_CTRL [RM0440 Rev 9, Sec. 47.13, p. 2104]. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t cycles_now(void)
{
    return DWT->CYCCNT;
}

uint32_t clock_hz(void)
{
    return core_hz;
}

/* Wait until (*reg & mask) == want, at most WAIT_CYCLES. */
static bool wait_bits(const volatile uint32_t *reg, uint32_t mask, uint32_t want)
{
    uint32_t start = cycles_now();

    /* Unsigned subtraction stays right when the counter wraps. */
    while ((*reg & mask) != want) {
        if ((cycles_now() - start) > WAIT_CYCLES) {
            return false;
        }
    }
    return true;
}

static void wait_cycles(uint32_t cycles)
{
    uint32_t start = cycles_now();

    while ((cycles_now() - start) < cycles) {
    }
}

static bool set_flash_latency(uint32_t latency)
{
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) | latency;
    /* Read back until the new value is in use [RM0440 Rev 9, Sec. 5.3.3, p. 191, step 2]. */
    return wait_bits(&FLASH->ACR, FLASH_ACR_LATENCY_Msk, latency);
}

static bool select_system_clock(uint32_t sw, uint32_t sws)
{
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | sw;
    return wait_bits(&RCC->CFGR, RCC_CFGR_SWS_Msk, sws);
}

/* A step towards 170 MHz did not finish: HSI16 as the system clock, PLL off, AHB / 1. The
 * 4 wait states and boost mode may stay: both are allowed at 16 MHz [RM0440 Rev 9, Table 29,
 * p. 190], and clock_back_to_reset() restores them before the handover. */
static void stay_on_hsi16(void)
{
    if (!select_system_clock(RCC_CFGR_SW_HSI, RCC_CFGR_SWS_HSI)) {
        core_hz = 0U;   /* source unknown: leave AHB divided by 2, the slower side */
        return;
    }
    RCC->CR &= ~RCC_CR_PLLON;
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;
    core_hz = HSI16_HZ;
}

bool clock_to_170mhz(void)
{
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
    if (!set_flash_latency(FLASH_ACR_LATENCY_4WS)) {
        stay_on_hsi16();   /* 170 MHz with fewer wait states would misread the flash */
        return false;
    }

    /* 4: PLL on, then make it the system clock. Fields can change only while it is off. */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | (PLLM_FIELD << RCC_PLLCFGR_PLLM_Pos) |
                   (PLL_N << RCC_PLLCFGR_PLLN_Pos) | (PLLR_DIV2_FIELD << RCC_PLLCFGR_PLLR_Pos) |
                   RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    if (!wait_bits(&RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY) ||
        !select_system_clock(RCC_CFGR_SW_PLL, RCC_CFGR_SWS_PLL)) {
        stay_on_hsi16();
        return false;
    }

    wait_cycles(SETTLE_CYCLES);                                         /* 5: wait 1 us, */
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;                                    /*    then AHB / 1 */
    core_hz = CORE_CLOCK_HZ;
    return true;
}

void clock_back_to_reset(void)
{
    /* Lowering the clock: source first, wait states after [RM0440 Rev 9, Sec. 5.3.3, p. 191].
     * If the source did not change, stop here: fewer wait states at 170 MHz would misread the
     * flash. The firmware then starts on the PLL and sets its own clock again. */
    if (!select_system_clock(RCC_CFGR_SW_HSI, RCC_CFGR_SWS_HSI)) {
        return;
    }
    RCC->CR &= ~RCC_CR_PLLON;
    (void)wait_bits(&RCC->CR, RCC_CR_PLLRDY, 0U);   /* the PLL stops within a few cycles */
    RCC->PLLCFGR = PLLCFGR_RESET_VALUE;
    FLASH->ACR &= ~FLASH_ACR_PRFTEN;
    (void)set_flash_latency(FLASH_ACR_LATENCY_1WS);
    /* Boost -> normal mode: set R1MODE; 16 MHz is far below the 150 MHz normal-mode limit
     * [RM0440 Rev 9, Sec. 6.1.5, p. 233]. R1MODE = 1 is also its reset value (p. 267). */
    PWR->CR5 |= PWR_CR5_R1MODE;
    RCC->APB1ENR1 &= ~RCC_APB1ENR1_PWREN;
    /* Cycle counter off too. A system reset would not do this: these debug registers are
     * reset only at power-on [RM0440 Rev 9, Sec. 47.10, p. 2102]. */
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
    CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;
}
