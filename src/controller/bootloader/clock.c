/*
 * clock.c — core clock up to 170 MHz and back, register level.
 * After reset the core runs from HSI16 at 16 MHz with 1 flash wait state
 * [RM0440 Rev 9, Sec. 5.3.3, p. 190].
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

static void wait_cycles(uint32_t cycles)
{
    uint32_t start = cycles_now();
    /* Unsigned subtraction stays right when the counter wraps. */
    while ((cycles_now() - start) < cycles) {
    }
}

static void set_flash_latency(uint32_t latency)
{
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY_Msk) | latency;
    /* Read back until the new value is in use [RM0440 Rev 9, Sec. 5.3.3, p. 191, step 2]. */
    while ((FLASH->ACR & FLASH_ACR_LATENCY_Msk) != latency) {
    }
}

static void select_system_clock(uint32_t sw, uint32_t sws)
{
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW_Msk) | sw;
    while ((RCC->CFGR & RCC_CFGR_SWS_Msk) != sws) {
    }
}

void clock_to_170mhz(void)
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
    set_flash_latency(FLASH_ACR_LATENCY_4WS);

    /* 4: PLL on, then make it the system clock. Fields can change only while it is off. */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_HSI | (PLLM_FIELD << RCC_PLLCFGR_PLLM_Pos) |
                   (PLL_N << RCC_PLLCFGR_PLLN_Pos) | (PLLR_DIV2_FIELD << RCC_PLLCFGR_PLLR_Pos) |
                   RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) == 0U) {
    }
    select_system_clock(RCC_CFGR_SW_PLL, RCC_CFGR_SWS_PLL);

    wait_cycles(SETTLE_CYCLES);                                         /* 5: wait 1 us, */
    RCC->CFGR &= ~RCC_CFGR_HPRE_Msk;                                    /*    then AHB / 1 */
}

void clock_back_to_reset(void)
{
    /* Lowering the clock: source first, wait states after [RM0440 Rev 9, Sec. 5.3.3, p. 191]. */
    select_system_clock(RCC_CFGR_SW_HSI, RCC_CFGR_SWS_HSI);
    RCC->CR &= ~RCC_CR_PLLON;
    while ((RCC->CR & RCC_CR_PLLRDY) != 0U) {
    }
    RCC->PLLCFGR = PLLCFGR_RESET_VALUE;
    FLASH->ACR &= ~FLASH_ACR_PRFTEN;
    set_flash_latency(FLASH_ACR_LATENCY_1WS);
    /* Boost -> normal mode: set R1MODE; 16 MHz is far below the 150 MHz normal-mode limit
     * [RM0440 Rev 9, Sec. 6.1.5, p. 233]. R1MODE = 1 is also its reset value (p. 267). */
    PWR->CR5 |= PWR_CR5_R1MODE;
    RCC->APB1ENR1 &= ~RCC_APB1ENR1_PWREN;
    /* Cycle counter off too. A system reset would not do this: these debug registers are
     * reset only at power-on [RM0440 Rev 9, Sec. 47.10, p. 2102]. */
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
    CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;
}
